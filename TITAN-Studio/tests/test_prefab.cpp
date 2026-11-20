// TITAN prefabs: serialization, validation, subtree capture, instantiation. Original TITAN code.
#include "titan/assets.h"
#include "titan/clipboard.h"
#include "titan/prefab.h"
#include "titan_test.h"

#include <fstream>
#include <set>
#include <unistd.h>

using namespace titan;
namespace fs = std::filesystem;

namespace {
// root "Tower" with two children; one child has its own child.
Scene towerScene(ObjectId& root) {
    Scene s;
    auto add = [&](const std::string& name, ObjectId parent, Vec3 pos) {
        SceneObject o;
        o.id = s.allocateId();
        o.name = name;
        o.parent = parent;
        o.transform.position = pos;
        s.put(o);
        return o.id;
    };
    add("Unrelated", kNoObject, {9, 9, 9});
    root = add("Tower", kNoObject, {5, 0, 5});
    const ObjectId floor = add("Floor", root, {0, 1, 0});
    add("Lamp", floor, {0, 0.5, 0});
    add("Roof", root, {0, 3, 0});
    return s;
}
}  // namespace

TEST("a subtree becomes a prefab with local ids, parents first, and round-trips through JSON") {
    ObjectId root = 0;
    Scene s = towerScene(root);
    Prefab p = Prefab::fromSubtree(s, root, "Tower", newPrefabId());
    CHECK(p.objects.size() == 4);
    CHECK(p.objects[0].id == 1 && p.objects[0].parent == kNoObject && p.objects[0].name == "Tower");
    for (size_t i = 1; i < p.objects.size(); ++i) CHECK(p.objects[i].parent >= 1 && p.objects[i].parent < p.objects[i].id);
    const Prefab q = Prefab::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.id == p.id && q.name == "Tower" && q.objects.size() == 4);
    for (size_t i = 0; i < 4; ++i) CHECK(q.objects[i] == p.objects[i]);
}

TEST("prefab validation rejects malformed files") {
    ObjectId root = 0;
    Scene s = towerScene(root);
    const Json good = Prefab::fromSubtree(s, root, "Tower", newPrefabId()).toJson();
    auto mutate = [&](auto fn) { Json j = Json::parse(good.dump()); fn(j); return j; };
    CHECK_THROWS(Prefab::fromJson(mutate([](Json& j) { j["format"] = 2; })));
    CHECK_THROWS(Prefab::fromJson(mutate([](Json& j) { j["id"] = "XYZ"; })));
    CHECK_THROWS(Prefab::fromJson(mutate([](Json& j) { j["objects"] = Json::array(); })));
    CHECK_THROWS(Prefab::fromJson(mutate([](Json& j) { j["objects"].asArray()[0]["parent"] = 3; })));   // root has a parent
    CHECK_THROWS(Prefab::fromJson(mutate([](Json& j) { j["objects"].asArray()[2]["parent"] = 99; })));  // dangling parent
    CHECK_THROWS(Prefab::fromJson(mutate([](Json& j) { j["objects"].asArray()[1]["parent"] = 3; })));   // parent after child
    CHECK_THROWS(Prefab::fromJson(mutate([](Json& j) { j["objects"].asArray()[2]["id"] = 2; })));       // duplicate id
    CHECK_THROWS(Prefab::fromJson(mutate([](Json& j) { j["objects"].asArray()[3]["parent"] = 0; })));   // second root
}

TEST("instantiate allocates fresh ids, keeps the hierarchy, places the root and links every object") {
    ObjectId root = 0;
    Scene s = towerScene(root);
    const Prefab p = Prefab::fromSubtree(s, root, "Tower", newPrefabId());
    const ObjectId before = s.peekNextId();
    auto a = p.instantiate(s, kNoObject, Vec3{-3, 0, 2});
    for (const auto& o : a) s.put(o);
    auto b = p.instantiate(s);
    for (const auto& o : b) s.put(o);
    CHECK(a.size() == 4 && b.size() == 4);
    CHECK(a[0].id >= before && b[0].id > a.back().id);  // fresh, never reused
    CHECK(a[0].transform.position == (Vec3{-3, 0, 2}));
    CHECK(b[0].transform.position == (Vec3{5, 0, 5}));    // default: prefab root's own position
    CHECK(s.childrenOf(a[0].id).size() == 2);
    for (size_t i = 0; i < 4; ++i) {
        CHECK(a[i].prefab && a[i].prefab->prefabId == p.id && a[i].prefab->localId == p.objects[i].id);
        CHECK(a[i].name == p.objects[i].name);
    }
    for (const auto& o : a)  // "Roof" (BFS order: Tower, Floor, Roof, Lamp) hangs under the new root
        if (o.name == "Roof") CHECK(s.find(o.id)->parent == a[0].id);
    // The scene (with links) survives its own JSON round trip.
    const Scene back = Scene::fromJson(s.toJson());
    CHECK(back.find(a[2].id)->prefab == a[2].prefab);
}

TEST("prefabs are assets: the asset pipeline imports valid ones and reports broken ones") {
    const auto root = fs::temp_directory_path() / ("titan-prefab-assets-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "Assets/Prefabs");
    ObjectId r = 0;
    Scene s = towerScene(r);
    std::ofstream(root / "Assets/Prefabs/Tower.tprefab") << Prefab::fromSubtree(s, r, "Tower", newPrefabId()).toJson().dump();
    std::ofstream(root / "Assets/Prefabs/Broken.tprefab") << R"({"format":1,"id":"nothex","name":"x","objects":[]})";
    assets::AssetDatabase db(root);
    db.refresh();
    const auto* tower = db.findByPath("Prefabs/Tower.tprefab");
    CHECK(tower && tower->status == assets::Status::Ok && tower->metadata["objects"].asNumber() == 4);
    const auto* broken = db.findByPath("Prefabs/Broken.tprefab");
    CHECK(broken && broken->status == assets::Status::Failed && broken->error.find("invalid prefab") != std::string::npos);
    fs::remove_all(root);
}

TEST("clipboard: copy keeps the hierarchy with local ids, world-space roots, and round-trips") {
    ObjectId root = 0;
    Scene s = towerScene(root);
    const ObjectId floor = s.childrenOf(root)[0];
    const ObjectId lamp = s.childrenOf(floor)[0];
    // Tower listed with its own descendant Lamp: Lamp comes along once, not as a second root.
    Clipboard c = Clipboard::copy(s, {root, lamp}, "/p");
    CHECK(c.objects.size() == 4 && c.objects[0].parent == kNoObject && c.objects[0].name == "Tower");
    for (size_t i = 1; i < c.objects.size(); ++i) CHECK(c.objects[i].parent >= 1 && c.objects[i].parent < c.objects[i].id);
    // A child copied alone becomes a root carrying its world position (Tower 5,0,5 + Floor 0,1,0).
    Clipboard f = Clipboard::copy(s, {floor}, "/p");
    CHECK(f.objects.size() == 2 && f.objects[0].transform.position == Vec3(5, 1, 5));
    const Clipboard back = Clipboard::fromJson(Json::parse(c.toJson().dump()));
    CHECK(back.objects.size() == 4);
    for (size_t i = 0; i < 4; ++i) CHECK(back.objects[i] == c.objects[i]);
    CHECK_THROWS(Clipboard::copy(s, {999}, "/p"));
    CHECK_THROWS(Clipboard::copy(s, {}, "/p"));
}

TEST("clipboard: paste allocates fresh ids, never links outside the paste, and handles parents") {
    ObjectId root = 0;
    Scene s = towerScene(root);
    Clipboard c = Clipboard::copy(s, {root}, "/p");
    const size_t before = s.size();
    auto objs = c.paste(s, kNoObject, true);
    CHECK(objs.size() == 4);
    std::set<ObjectId> fresh;
    for (const auto& o : objs) {
        CHECK(!s.contains(o.id));  // allocated, not inserted, and not an existing id
        fresh.insert(o.id);
    }
    for (size_t i = 1; i < objs.size(); ++i) CHECK(fresh.count(objs[i].parent) == 1);
    for (const auto& o : objs) s.put(o);
    CHECK(s.size() == before + 4);
    CHECK(objs[0].transform.position == Vec3(5, 0, 5));
    // Under a parent at (1,0,0) the root keeps its world position.
    SceneObject holder;
    holder.id = s.allocateId();
    holder.name = "Holder";
    holder.transform.position = {1, 0, 0};
    s.put(holder);
    auto under = c.paste(s, holder.id, true);
    CHECK(under[0].parent == holder.id && under[0].transform.position == Vec3(4, 0, 5));
    CHECK_THROWS(c.paste(s, 999999, true));
    // Prefab links survive only within the same project; provenance never.
    Clipboard p = c;
    p.objects[0].prefab = PrefabLink{newPrefabId(), 1};
    CHECK(p.paste(s, kNoObject, true)[0].prefab.has_value());
    CHECK(!p.paste(s, kNoObject, false)[0].prefab.has_value());
}

TEST("clipboard: validation rejects foreign, future, and malformed content") {
    ObjectId root = 0;
    Scene s = towerScene(root);
    const Json good = Clipboard::copy(s, {root}, "/p").toJson();
    CHECK_THROWS(Clipboard::fromJson(Json::parse("{\"format\":\"other\"}")));
    Json future = good;
    future["version"] = 2;
    CHECK_THROWS(Clipboard::fromJson(future));
    Json cyclic = good;
    Json objs = Json::array();
    for (size_t i = 0; i < good["objects"].size(); ++i) {
        Json o = good["objects"].at(i);
        if (i == 1) o["parent"] = 3.0;  // forward link
        objs.push_back(o);
    }
    cyclic["objects"] = objs;
    CHECK_THROWS(Clipboard::fromJson(cyclic));
    Json empty = good;
    empty["objects"] = Json::array();
    CHECK_THROWS(Clipboard::fromJson(empty));
}

TEST("clipboard: sheared placements are refused, never copied or pasted with the wrong transform") {
    Scene s;
    auto add = [&](const std::string& name, ObjectId parent, Transform t) {
        SceneObject o;
        o.id = s.allocateId();
        o.name = name;
        o.parent = parent;
        o.transform = t;
        s.put(o);
        return o.id;
    };
    const ObjectId squash = add("Squash", kNoObject, Transform{{0, 0, 0}, {0, 0, 0}, {3, 1, 1}});
    const ObjectId tilted = add("Tilted", squash, Transform{{1, 0, 0}, {0, 0, 45}, {1, 1, 1}});
    // Copy: the rotated child's world matrix (parent scale 3,1,1) has no T*R*S form.
    CHECK_THROWS(Clipboard::copy(s, {tilted}, "/p"));
    // With its parent it is fine: the child stays local under the copied parent.
    CHECK(Clipboard::copy(s, {squash}, "/p").objects.size() == 2);
    // Paste: a rotated root under the non-uniformly scaled parent is refused before any id is allocated.
    const ObjectId loose = add("Loose", kNoObject, Transform{{0, 0, 0}, {0, 0, 45}, {1, 1, 1}});
    const Clipboard c = Clipboard::copy(s, {loose}, "/p");
    const ObjectId next = s.peekNextId();
    const size_t count = s.size();
    CHECK_THROWS(c.paste(s, squash, true));
    CHECK(s.peekNextId() == next && s.size() == count);
    CHECK(c.paste(s, kNoObject, true).size() == 1);  // at the scene root it lands as copied
}

TITAN_TEST_MAIN()
