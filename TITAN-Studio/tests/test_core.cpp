// TITAN core unit tests: JSON, math, picking, scene model, undo/redo, persistence, framing.
#include "titan/capabilities.h"
#include "titan_test.h"

#include <set>

#include "titan/components.h"
#include "titan/edit.h"
#include "titan/json.h"
#include "titan/detail/json_number.h"
#include "titan/mesh.h"
#include "titan/play_session.h"
#include "titan/project.h"
#include "titan/protocol.h"
#include "titan/scene.h"

#include <charconv>
#include <clocale>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <unistd.h>

using namespace titan;
namespace fs = std::filesystem;

static fs::path tempDir(const std::string& tag) {
    fs::path p = fs::temp_directory_path() / ("titan-test-" + tag + "-" + std::to_string(::getpid()));
    fs::remove_all(p);
    return p;
}

// ---- JSON ------------------------------------------------------------------------------
TEST("json: round trip preserves values, order and unicode") {
    Json j = Json::parse(R"({"b":1,"a":[true,false,null,-2.5e3,"x\u00e9\ud83d\ude00\n"],"c":{"d":0.1}})");
    CHECK(j["b"].asNumber() == 1);
    CHECK(j["a"].at(3).asNumber() == -2500);
    CHECK(j["a"].at(4).asString() == "x\xC3\xA9\xF0\x9F\x98\x80\n");
    CHECK(j["c"]["d"].asNumber() == 0.1);
    CHECK(j.asObject().front().first == "b");  // insertion order kept
    Json again = Json::parse(j.dump());
    CHECK(again == j);
    CHECK(Json::parse(j.dump(2)) == j);
}

TEST("json: Arabic text survives round trip") {
    Json j = Json::object();
    j["name"] = "مشروع تيتان";
    CHECK(Json::parse(j.dump())["name"].asString() == "مشروع تيتان");
}

TEST("json: rejects malformed input and excessive nesting") {
    for (const char* bad : {"", "{", "[1,]", "{\"a\" 1}", "tru", "01", "1.", "\"\\x\"", "\"a", "[1] 2", "\"\\ud800\""})
        CHECK_THROWS(Json::parse(bad));
    std::string deep(200, '[');
    deep += std::string(200, ']');
    CHECK_THROWS(Json::parse(deep));
    CHECK(Json::parse(std::string(100, '[') + std::string(100, ']')).isArray());
}

TEST("json: type mismatches throw, defaults apply") {
    Json j = Json::parse(R"({"n":3,"s":"t"})");
    CHECK_THROWS(j["s"].asNumber());
    CHECK_THROWS(j["n"].asString());
    CHECK(j.getNumber("missing", 7) == 7);
    CHECK(j.getString("missing", "d") == "d");
    CHECK(j["missing"].isNull());
    CHECK_THROWS(Json(2.5).asInt());
    CHECK_THROWS(Json(-1).asUInt());
}

TEST("json: integers print without exponent, non-finite becomes null") {
    CHECK(Json(42).dump() == "42");
    CHECK(Json(9007199254740991.0).dump() == "9007199254740991");
    CHECK(Json(std::nan("")).dump() == "null");
}

// ---- json numbers (locale-independent parsing; std::from_chars or the strtod_l fallback) ----
namespace {
uint64_t bitsOf(double d) {
    uint64_t b;
    std::memcpy(&b, &d, sizeof b);
    return b;
}
double fromBits(uint64_t b) {
    double d;
    std::memcpy(&d, &b, sizeof d);
    return d;
}
using titan::detail::NumberParse;
using NumberFn = NumberParse (*)(const char*, const char*, double&);
std::vector<std::pair<const char*, NumberFn>> numberParsers() {
    std::vector<std::pair<const char*, NumberFn>> v{{"active", &titan::detail::parseJsonDouble}};
#if TITAN_HAS_STRTOD_L_FALLBACK
    v.push_back({"strtod_l", &titan::detail::parseJsonDoubleStrtod});
#endif
#if TITAN_HAS_FLOAT_FROM_CHARS
    v.push_back({"from_chars", &titan::detail::parseJsonDoubleFromChars});
#endif
    return v;
}
NumberParse parseWith(NumberFn fn, const std::string& s, double& d) { return fn(s.data(), s.data() + s.size(), d); }
}  // namespace

TEST("json number: exact results at the edges of double (every parser in this build)") {
    std::printf("         json number parser in this build: %s\n", titan::detail::jsonDoubleParserName());
    struct Case { const char* text; NumberParse r; uint64_t bits; };
    const Case cases[] = {
        {"0", NumberParse::Ok, 0x0000000000000000ull},
        {"-0", NumberParse::Ok, 0x8000000000000000ull},
        {"0e999999", NumberParse::Ok, 0},
        {"1", NumberParse::Ok, 0x3FF0000000000000ull},
        {"-1.5", NumberParse::Ok, 0xBFF8000000000000ull},
        {"0.1", NumberParse::Ok, 0x3FB999999999999Aull},
        {"9007199254740993", NumberParse::Ok, 0x4340000000000000ull},  // halfway: ties to even (2^53)
        {"1.00000000000000011102230246251565404236316680908203125", NumberParse::Ok, 0x3FF0000000000000ull},  // exact halfway
        {"1.00000000000000011102230246251565404236316680908203126", NumberParse::Ok, 0x3FF0000000000001ull},  // just above
        {"1.7976931348623157e308", NumberParse::Ok, 0x7FEFFFFFFFFFFFFFull},  // DBL_MAX
        {"1.7976931348623158e308", NumberParse::Ok, 0x7FEFFFFFFFFFFFFFull},  // rounds down to DBL_MAX
        {"2.2250738585072014e-308", NumberParse::Ok, 0x0010000000000000ull},  // DBL_MIN
        {"2.2250738585072011e-308", NumberParse::Ok, 0x000FFFFFFFFFFFFFull},  // largest subnormal
        {"4.9406564584124654e-324", NumberParse::Ok, 0x0000000000000001ull},  // smallest subnormal
        {"2.4703282292062328e-324", NumberParse::Ok, 0x0000000000000001ull},  // just above half of it
        {"-1e-320", NumberParse::Ok, 0x80000000000007E8ull},
        {"1.7976931348623159e308", NumberParse::OutOfRange, 0},  // overflow
        {"1e400", NumberParse::OutOfRange, 0},
        {"-1e400", NumberParse::OutOfRange, 0},
        {"2.4703282292062327e-324", NumberParse::OutOfRange, 0},  // underflow to zero
        {"1e-400", NumberParse::OutOfRange, 0},
        {"-1e-400", NumberParse::OutOfRange, 0},
    };
    for (const auto& [name, fn] : numberParsers()) {
        for (const Case& c : cases) {
            double d = 12345;
            const NumberParse r = parseWith(fn, c.text, d);
            if (r != c.r || (r == NumberParse::Ok && bitsOf(d) != c.bits))
                throw titan_test::Failure(std::string(name) + " parser: wrong result for " + c.text);
        }
        // 800 significant digits (strtod_l fallback switches to a heap buffer).
        std::string longNum = "1." + std::string(799, '3');
        double d = 0;
        CHECK(parseWith(fn, longNum, d) == NumberParse::Ok && bitsOf(d) == bitsOf(1.3333333333333333));
    }
}

TEST("json number: invalid text is rejected, never partially parsed") {
    for (const auto& [name, fn] : numberParsers()) {
        for (const char* bad : {"", "-", "+1", " 1", "1 ", "1e", "1e+", "--1", "0x10", "inf", "-inf", "nan",
                                "infinity", "1,5", "1e5.5", ".", "e5", "1..2"}) {
            double d = 7;
            if (parseWith(fn, bad, d) == NumberParse::Ok)
                throw titan_test::Failure(std::string(name) + " parser accepted \"" + bad + "\"");
            CHECK(d == 7);  // output untouched on failure
        }
    }
    // The JSON grammar is stricter still (and is checked before the number parser runs).
    for (const char* bad : {"1e400", "-1e-400", "+1", ".5", "1.e3", "0x1", "Infinity", "NaN"}) CHECK_THROWS(Json::parse(bad));
}

#if TITAN_HAS_FLOAT_FROM_CHARS && TITAN_HAS_STRTOD_L_FALLBACK
TEST("json number: strtod_l fallback matches std::from_chars bit for bit (300k inputs)") {
    std::mt19937_64 rng(0x7174A9);
    char buf[128];
    auto same = [](const std::string& s) {
        double a = 0, b = 0;
        const NumberParse ra = parseWith(&titan::detail::parseJsonDoubleFromChars, s, a);
        const NumberParse rb = parseWith(&titan::detail::parseJsonDoubleStrtod, s, b);
        if (ra != rb || (ra == NumberParse::Ok && bitsOf(a) != bitsOf(b)))
            throw titan_test::Failure("fallback differs from from_chars for " + s);
    };
    for (int i = 0; i < 100000; ++i) {  // every exponent range: random bit patterns, three spellings each
        const double d = fromBits(rng());
        if (!std::isfinite(d)) continue;
        same(std::string(buf, std::to_chars(buf, buf + sizeof buf, d).ptr));
        std::snprintf(buf, sizeof buf, "%.17g", d);
        same(buf);
        std::snprintf(buf, sizeof buf, "%.25e", d);
        same(buf);
    }
    std::uniform_int_distribution<int> len(1, 40), digit(0, 9), expo(-360, 330);
    for (int i = 0; i < 100000; ++i) {  // random decimal strings, including overflow/underflow and halfway cases
        std::string s = (i & 1) ? "-" : "";
        const int n = len(rng);
        for (int k = 0; k < n; ++k) s += char('0' + digit(rng));
        if (n > 1 && (i & 2)) s.insert(s.size() - size_t(n / 2), ".");
        s += "e" + std::to_string(expo(rng));
        same(s);
    }
}
#endif

TEST("json number: round trip is bit-exact through Json::parse and dump (200k doubles)") {
    std::mt19937_64 rng(0xD0B1E);
    for (int i = 0; i < 200000; ++i) {
        const double d = fromBits(rng());
        if (!std::isfinite(d) || d == 0) continue;  // dump writes -0 as "0" (integer form)
        Json arr = Json::array();
        arr.push_back(Json(d));
        const double back = Json::parse(arr.dump()).at(0).asNumber();
        if (bitsOf(back) != bitsOf(d)) throw titan_test::Failure("round trip changed " + arr.dump());
    }
}

TEST("json number: parsing does not depend on the C locale") {
    const char* used = nullptr;
    for (const char* name : {"de_DE.UTF-8", "fr_FR.UTF-8", "ar_SA.UTF-8", "de_DE"}) {
        if (std::setlocale(LC_NUMERIC, name)) { used = name; break; }
    }
    std::printf("         decimal-comma locale available: %s\n", used ? used : "none (checked under \"C\" only)");
    for (const auto& [name, fn] : numberParsers()) {
        double d = 0;
        CHECK(parseWith(fn, "1.5", d) == NumberParse::Ok && d == 1.5);
        CHECK(parseWith(fn, "1,5", d) != NumberParse::Ok);
    }
    CHECK(Json::parse("[2.25]").at(0).asNumber() == 2.25);
    CHECK(Json::parse(Json(0.1).dump()).asNumber() == 0.1);
    std::setlocale(LC_NUMERIC, "C");
}

// ---- math & picking --------------------------------------------------------------------
TEST("math: transform matrix and inverse") {
    Transform t{{1, 2, 3}, {10, 45, -20}, {2, 0.5f, 3}};
    Mat4 m = t.matrix();
    auto inv = m.inverse();
    CHECK(inv.has_value());
    Vec3 p{0.3f, -0.7f, 1.1f};
    Vec3 back = inv->transformPoint(m.transformPoint(p));
    CHECK_NEAR(back.x, p.x, 1e-4);
    CHECK_NEAR(back.y, p.y, 1e-4);
    CHECK_NEAR(back.z, p.z, 1e-4);
    Vec3 o = m.transformPoint({0, 0, 0});
    CHECK_NEAR(o.x, 1, 1e-6);
    CHECK_NEAR(o.y, 2, 1e-6);
    CHECK_NEAR(o.z, 3, 1e-6);
    // Yaw +90 maps +X to -Z (right-handed, Y-up)
    Vec3 d = Transform{{0, 0, 0}, {0, 90, 0}, {1, 1, 1}}.matrix().transformDirection({1, 0, 0});
    CHECK_NEAR(d.z, -1, 1e-6);
}

TEST("math: Transform::fromMatrix inverts matrix() and refuses shear") {
    const Transform cases[] = {{{1, 2, 3}, {10, 45, -20}, {2, 0.5f, 3}},
                               {{-4, 0, 7}, {-80, 170, 35}, {1, 1, 1}},
                               {{0, 0, 0}, {90, 30, 0}, {1, 2, 1}},  // gimbal lock
                               {{0, 1, 0}, {0, 0, 0}, {-1, 1, 1}}};  // mirrored
    for (const auto& t : cases) {
        auto back = Transform::fromMatrix(t.matrix());
        CHECK(back.has_value());
        const Mat4 a = t.matrix(), b = back->matrix();
        for (int i = 0; i < 16; ++i) CHECK_NEAR(a.m[size_t(i)], b.m[size_t(i)], 1e-4);
    }
    auto plain = Transform::fromMatrix(Transform{{1, 2, 3}, {0, 90, 0}, {2, 2, 2}}.matrix());
    CHECK(plain && plain->rotation.y == 90 && plain->scale.x == 2 && plain->position.z == 3);  // snapped exactly
    // A rotated child under a non-uniformly scaled parent is sheared: not expressible as T*R*S.
    const Mat4 sheared = Transform{{0, 0, 0}, {0, 0, 0}, {3, 1, 1}}.matrix() * Transform{{0, 0, 0}, {0, 0, 45}, {1, 1, 1}}.matrix();
    CHECK(!Transform::fromMatrix(sheared));
    CHECK(!Transform::fromMatrix(Transform{{0, 0, 0}, {0, 0, 0}, {1, 0, 1}}.matrix()));
}

TEST("picking: exact primitive intersections") {
    CHECK_NEAR(*intersectShape(Shape::Cube, {{0, 0, 5}, {0, 0, -1}}), 4.5, 1e-6);
    CHECK_NEAR(*intersectShape(Shape::Sphere, {{0, 0, 5}, {0, 0, -1}}), 4.5, 1e-6);
    CHECK(!intersectShape(Shape::Sphere, {{0.45f, 0.45f, 5}, {0, 0, -1}}));  // inside cube corner, outside sphere
    CHECK(intersectShape(Shape::Cube, {{0.45f, 0.45f, 5}, {0, 0, -1}}));
    CHECK_NEAR(*intersectShape(Shape::Cylinder, {{0, 5, 0}, {0, -1, 0}}), 4.5, 1e-6);  // cap
    CHECK_NEAR(*intersectShape(Shape::Plane, {{0.2f, 3, 0.1f}, {0, -1, 0}}), 3, 1e-6);
    CHECK(!intersectShape(Shape::Plane, {{0.7f, 3, 0}, {0, -1, 0}}));
    CHECK(!intersectShape(Shape::None, {{0, 0, 5}, {0, 0, -1}}));
}

TEST("picking: nearest visible object in world space, respecting hierarchy") {
    Scene s = Scene::makeDefault("T");
    // Looking down -Z from z=20 at height 1: Hero Cube (2m at origin) is the first hit.
    auto hit = pick(s, {{0, 1, 20}, {0, 0, -1}});
    CHECK(hit.has_value());
    CHECK(s.find(hit->id)->name == "Hero Cube");
    CHECK_NEAR(hit->distance, 19.0, 0.35);  // cube is rotated 20 deg about Y
    // Crate C sits inside group "Props" (z=-4) at local (-0.4, 1.5, 0.1): world (-0.4, 1.5, -3.9)
    auto crate = pick(s, {{-0.4f, 10, -3.9f}, {0, -1, 0}});
    CHECK(crate && s.find(crate->id)->name == "Crate C");
    // Hidden objects are ignored.
    SceneObject cube = *s.find(hit->id);
    cube.visible = false;
    s.put(cube);
    auto hit2 = pick(s, {{0, 1, 20}, {0, 0, -1}});
    CHECK(hit2 && hit2->id != cube.id);
}

TEST("mesh: primitives are well formed") {
    for (Shape sh : {Shape::Cube, Shape::Plane, Shape::Sphere, Shape::Cylinder}) {
        MeshData m = makeMesh(sh);
        CHECK(m.vertexCount() > 0);
        CHECK(m.indices.size() % 3 == 0);
        for (uint32_t i : m.indices) CHECK(i < m.vertexCount());
        for (size_t v = 0; v < m.vertexCount(); ++v)
            for (int k = 0; k < 3; ++k) CHECK(std::fabs(m.vertices[v * 6 + size_t(k)]) <= 0.5f + 1e-5f);
    }
    CHECK(makeUnitBoxLines().indices.size() == 24);
}

// ---- scene model -------------------------------------------------------------------------
TEST("scene: serialization round trip and validation") {
    Scene s = Scene::makeDefault("Main");
    s.putRecipe({"scatter-1", "titan.procedural.scatter", Json::parse(R"({"seed":7})")});
    Scene r = Scene::fromJson(Json::parse(s.toJson().dump(2)));
    CHECK(r.size() == s.size());
    for (const auto& [id, o] : s.objects()) CHECK(*r.find(id) == o);
    CHECK(r.peekNextId() == s.peekNextId());
    CHECK(r.findRecipe("scatter-1")->params["seed"].asNumber() == 7);

    Json bad = s.toJson();
    bad["formatVersion"] = 99;
    CHECK_THROWS(Scene::fromJson(bad));
    Json dup = s.toJson();
    dup["objects"].asArray().push_back(dup["objects"].at(0));
    CHECK_THROWS(Scene::fromJson(dup));
    Json orphan = s.toJson();
    orphan["objects"].asArray()[0]["parent"] = 9999;
    CHECK_THROWS(Scene::fromJson(orphan));
}

TEST("scene: rejects parent cycles and keeps ids monotonic") {
    Scene s = Scene::makeDefault("Main");
    ObjectId group = 0;
    for (const auto& [id, o] : s.objects())
        if (o.type == ObjectType::Group) group = id;
    CHECK(group != 0);
    ObjectId child = s.childrenOf(group).front();
    SceneObject g = *s.find(group);
    g.parent = child;
    CHECK_THROWS(s.put(g));
    ObjectId a = s.allocateId(), b = s.allocateId();
    CHECK(b == a + 1);
    auto all = s.withDescendants({group});
    CHECK(all.front() == group && all.size() == 4);
}

// ---- undo / redo -------------------------------------------------------------------------
static SceneEdit moveEdit(const Scene& s, ObjectId id, float x, const std::string& key = {}) {
    SceneObject o = *s.find(id);
    o.transform.position.x = x;
    SceneEdit e("Move", key);
    e.recordUpdate(s, o);
    return e;
}

TEST("undo: apply, undo, redo restore exact state") {
    Scene s = Scene::makeDefault("Main");
    const Json original = s.toJson();
    UndoStack u;
    ObjectId id = s.objects().begin()->first;
    u.push(s, moveEdit(s, id, 5));
    CHECK(s.find(id)->transform.position.x == 5);
    CHECK(u.isDirty());
    u.undo(s);
    CHECK(s.toJson() == original);
    CHECK(!u.isDirty());
    u.redo(s);
    CHECK(s.find(id)->transform.position.x == 5);
}

TEST("undo: create/delete with descendants is reversible") {
    Scene s = Scene::makeDefault("Main");
    const Json original = s.toJson();
    UndoStack u;
    ObjectId group = 0;
    for (const auto& [id, o] : s.objects())
        if (o.type == ObjectType::Group) group = id;
    SceneEdit del("Delete");
    for (ObjectId id : s.withDescendants({group})) del.recordDelete(s, id);
    u.push(s, std::move(del));
    CHECK(!s.contains(group));
    u.undo(s);
    CHECK(s.toJson()["objects"] == original["objects"]);
}

TEST("undo: coalescing merges a drag, but never across save or undo") {
    Scene s = Scene::makeDefault("Main");
    UndoStack u;
    ObjectId id = s.objects().begin()->first;
    float x0 = s.find(id)->transform.position.x;
    for (int i = 1; i <= 10; ++i) u.push(s, moveEdit(s, id, float(i), "drag-1"));
    CHECK(u.depth() == 1);
    u.undo(s);
    CHECK(s.find(id)->transform.position.x == x0);
    u.redo(s);
    u.markSaved();
    u.push(s, moveEdit(s, id, 20, "drag-1"));
    CHECK(u.depth() == 2);  // save point not merged away
    CHECK(u.isDirty());
    u.undo(s);
    CHECK(!u.isDirty());
}

TEST("undo: dirty tracking when the saved state becomes unreachable") {
    Scene s = Scene::makeDefault("Main");
    UndoStack u;
    ObjectId id = s.objects().begin()->first;
    u.push(s, moveEdit(s, id, 1));
    u.markSaved();
    u.undo(s);
    CHECK(u.isDirty());
    u.push(s, moveEdit(s, id, 2));  // discards redo branch holding the saved state
    CHECK(u.isDirty());
    u.undo(s);
    CHECK(u.isDirty());
}

// ---- project persistence ----------------------------------------------------------------
TEST("project: create, edit, save, reload") {
    fs::path dir = tempDir("project") / "مشروع";  // non-ASCII path on purpose
    ProjectInfo p = createProject(dir, "Demo");
    CHECK(fs::exists(p.manifestFile()) && fs::exists(p.sceneFile()));
    CHECK_THROWS(createProject(dir, "Again"));  // not empty
    Scene s = loadScene(readProject(dir));
    ObjectId id = s.objects().begin()->first;
    SceneObject o = *s.find(id);
    o.transform.position = {1.25f, 2.5f, -3.75f};
    s.put(o);
    saveScene(p, s);
    Scene r = loadScene(readProject(dir));
    CHECK(r.find(id)->transform.position == (Vec3{1.25f, 2.5f, -3.75f}));
    CHECK(!fs::exists(p.sceneFile().string() + ".tmp"));
    fs::remove_all(dir.parent_path());
}

TEST("project: manifest cannot point outside the project") {
    fs::path dir = tempDir("escape");
    createProject(dir, "Demo");
    Json m = readJsonFile(dir / "project.titan");
    m["defaultScene"] = "../../etc/passwd";
    writeFileAtomic(dir / "project.titan", m.dump());
    CHECK_THROWS(readProject(dir));
    fs::remove_all(dir);
}

// ---- framing -------------------------------------------------------------------------------
TEST("protocol: frame decoder handles fragmentation and oversize frames") {
    std::string stream;
    for (int i = 0; i < 50; ++i) stream += protocol::encodeFrame(protocol::makeRequest(uint64_t(i), "x"));
    std::mt19937 rng(1234);
    protocol::FrameDecoder d;
    std::vector<std::string> got;
    for (size_t off = 0; off < stream.size();) {
        size_t n = std::min(stream.size() - off, size_t(1 + rng() % 17));
        d.feed(stream.data() + off, n);
        off += n;
        while (auto f = d.next()) got.push_back(*f);
    }
    CHECK(got.size() == 50);
    CHECK(Json::parse(got[49])["id"].asNumber() == 49);

    protocol::FrameDecoder big;
    const char hdr[4] = {char(0x7F), char(0xFF), char(0xFF), char(0xFF)};
    big.feed(hdr, 4);
    CHECK(!big.next());
    CHECK(big.failed());
}

TEST("grid snapping rounds x and z only, and is off for non-positive steps") {
    const Vec3 p{1.26f, 0.37f, -0.74f};
    const Vec3 s = p.snappedXZ(0.5f);
    CHECK_NEAR(s.x, 1.5, 1e-6);
    CHECK_NEAR(s.y, 0.37, 1e-6);
    CHECK_NEAR(s.z, -0.5, 1e-6);
    CHECK(p.snappedXZ(0.0f) == p && p.snappedXZ(-1.0f) == p);
    CHECK_NEAR(Vec3({2.249f, 0, 0}).snappedXZ(0.25f).x, 2.25, 1e-6);
}

TEST("capability registry: unique names, each method in at most one capability, positive versions") {
    std::set<std::string> names, methods;
    for (const auto& c : titan::capabilityRegistry()) {
        CHECK(names.insert(c.name).second);
        CHECK(c.version >= 1);
        for (const char* m : c.methods) {
            CHECK(std::string(m).find('.') != std::string::npos);
            CHECK(methods.insert(m).second);
            CHECK(titan::capabilityForMethod(m) == &c);
        }
    }
    CHECK(titan::capabilityForMethod("object.update") == nullptr);  // base protocol
    CHECK(titan::findCapability("assets") && !titan::findCapability("nope"));
}

// ---- components --------------------------------------------------------------------------
TEST("components: objects without components serialize exactly as before") {
    SceneObject o;
    o.id = 7;
    o.name = "Plain";
    CHECK(!o.toJson().contains("components"));
    CHECK(SceneObject::fromJson(o.toJson()).components.empty());
}

TEST("components: known and unknown types round-trip; malformed lists are refused") {
    SceneObject o;
    o.id = 3;
    o.name = "Tagged";
    Component tags;
    tags.type = "titan.tags";
    tags.data["tags"] = Json(Json::Array{Json("enemy"), Json("boss")});
    Component future;
    future.type = "studio.future";
    future.version = 4;
    future.data["anything"] = 42;
    o.components.push_back(tags);
    o.components.push_back(future);
    const SceneObject back = SceneObject::fromJson(Json::parse(o.toJson().dump()));
    CHECK(back == o);
    CHECK(back.components.size() == 2 && back.components[1].type == "studio.future" && back.components[1].version == 4);
    auto withComponents = [&](const char* text) {
        Json j = o.toJson();
        j["components"] = Json::parse(text);
        return j;
    };
    CHECK_THROWS(SceneObject::fromJson(withComponents("{}")));
    CHECK_THROWS(SceneObject::fromJson(withComponents(R"([{"type":""}])")));
    CHECK_THROWS(SceneObject::fromJson(withComponents(R"([{"type":"a","version":0}])")));
    CHECK_THROWS(SceneObject::fromJson(withComponents(R"([{"type":"a","version":1.5}])")));
    CHECK_THROWS(SceneObject::fromJson(withComponents(R"([{"type":"a","data":[]}])")));
    CHECK_THROWS(SceneObject::fromJson(withComponents(R"([{"type":"a"},{"type":"a"}])")));
}

TEST("components: the registry validates, migrates older versions and refuses newer ones") {
    ComponentRegistry reg = ComponentRegistry::withBuiltins();
    Component tags;
    tags.type = "titan.tags";
    tags.data["tags"] = Json(Json::Array{Json("a"), Json("b")});
    CHECK(reg.normalize(tags) == tags);
    Component dup = tags;
    dup.data["tags"] = Json(Json::Array{Json("a"), Json("a")});
    CHECK_THROWS(reg.normalize(dup));
    Component missing;
    missing.type = "titan.tags";
    CHECK_THROWS(reg.normalize(missing));
    Component newer = tags;
    newer.version = 2;
    CHECK_THROWS(reg.normalize(newer));
    Component unknown;
    unknown.type = "vendor.thing";
    unknown.version = 9;
    CHECK(reg.normalize(unknown) == unknown);  // passed through untouched
    // test.mover v1 stored a number; v2 stores {"speed": {"value": n, "unit": "m/s"}}.
    ComponentType mover;
    mover.type = "test.mover";
    mover.version = 2;
    mover.validate = [](const Json& d) {
        if (!d["speed"].isObject()) throw std::invalid_argument("speed must be an object");
    };
    mover.migrate = [](const Json& d, int from) {
        if (from != 1 || !d["speed"].isNumber()) throw std::invalid_argument("cannot migrate");
        Json speed = Json::object();
        speed["value"] = d["speed"].asNumber();
        speed["unit"] = "m/s";
        Json out = Json::object();
        out["speed"] = speed;
        return out;
    };
    reg.add(mover);
    CHECK_THROWS(reg.add(mover));  // duplicate
    ComponentType noMigration;
    noMigration.type = "test.broken";
    noMigration.version = 3;
    CHECK_THROWS(reg.add(noMigration));
    Component old;
    old.type = "test.mover";
    old.data["speed"] = 2.5;
    const Component up = reg.normalize(old);
    CHECK(up.version == 2 && up.data["speed"]["value"].asNumber() == 2.5 && up.data["speed"]["unit"].asString() == "m/s");
    old.data["speed"] = "fast";
    CHECK_THROWS(reg.normalize(old));
}

TEST("components: normalizing a scene is all or nothing, and undo restores removed components") {
    ComponentRegistry reg = ComponentRegistry::withBuiltins();
    Scene s;
    Component tags;
    tags.type = "titan.tags";
    tags.data["tags"] = Json(Json::Array{Json("x")});
    SceneObject a;
    a.id = s.allocateId();
    a.name = "A";
    a.components.push_back(tags);
    s.put(a);
    Component bad = tags;
    bad.data["tags"] = 5;
    SceneObject b;
    b.id = s.allocateId();
    b.name = "B";
    b.components.push_back(bad);
    s.put(b);
    const std::string before = s.toJson().dump();
    CHECK_THROWS(reg.normalizeScene(s));
    CHECK(s.toJson().dump() == before);
    UndoStack undo;
    SceneObject stripped = *s.find(a.id);
    stripped.components.clear();
    SceneEdit edit("Remove component");
    edit.recordUpdate(s, stripped);
    undo.push(s, std::move(edit));
    CHECK(s.find(a.id)->components.empty());
    undo.undo(s);
    CHECK(s.find(a.id)->components.size() == 1 && s.find(a.id)->components[0] == tags);
}

// ---- edit/play ---------------------------------------------------------------------------
TEST("play: leaving play mode restores the edit scene exactly, whatever happened while playing") {
    Scene scene = Scene::makeDefault("Main");
    Component tags;
    tags.type = "titan.tags";
    tags.data["tags"] = Json(Json::Array{Json("start")});
    SceneObject tagged = *scene.find(scene.objects().begin()->first);
    tagged.components.push_back(tags);
    scene.put(tagged);
    const std::string editJson = scene.toJson().dump();
    const ObjectId nextBefore = scene.peekNextId();
    PlaySession play;
    CHECK(play.mode() == EditorMode::Edit && !play.playing() && play.editScene() == nullptr);
    CHECK_THROWS(play.end(scene));
    play.begin(scene);
    CHECK(play.playing() && play.mode() == EditorMode::Play && play.editScene() != nullptr);
    CHECK_THROWS(play.begin(scene));
    // Gameplay: move an object, drop its component, spawn one, delete one, change the environment.
    SceneObject moved = *scene.find(tagged.id);
    moved.transform.position = {40, 0, 0};
    moved.components.clear();
    scene.put(moved);
    SceneObject spawned;
    spawned.id = scene.allocateId();
    spawned.name = "Projectile";
    scene.put(spawned);
    ObjectId sun = kNoObject;
    for (const auto& entry : scene.objects())
        if (entry.second.name == "Sun") sun = entry.first;
    CHECK(sun != kNoObject);
    scene.erase(sun);
    scene.environment.ambient = 0.9f;
    for (int i = 0; i < 5; ++i) scene.bumpRevision();
    CHECK(play.editScene()->toJson().dump() == editJson);  // the saved copy is untouched while playing
    const uint64_t playRevision = scene.revision();
    Scene restored = play.end(scene);
    CHECK(!play.playing() && play.mode() == EditorMode::Edit);
    CHECK(restored.toJson().dump() == editJson);
    CHECK(restored.peekNextId() == nextBefore);  // ids allocated while playing are not consumed
    CHECK(restored.revision() > playRevision);
    // A second session behaves the same.
    play.begin(restored);
    CHECK(play.end(restored).toJson().dump() == editJson);
}

TITAN_TEST_MAIN()
