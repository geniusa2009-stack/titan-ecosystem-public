// TITAN Engine Host — session implementation. Original TITAN code.
#include "engine_session.h"

#include "titan/assets.h"
#include "titan/capabilities.h"
#include "titan/clipboard.h"
#include "titan/prefab.h"
#include "titan/scene_template.h"
#include "titan/protocol.h"
#include "titan/version.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <set>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace titan {

namespace {
struct ProtocolError : std::runtime_error {
    int code;
    ProtocolError(int c, const std::string& m) : std::runtime_error(m), code(c) {}
};

[[noreturn]] void fail(int code, const std::string& message) { throw ProtocolError(code, message); }

ObjectId requireId(const Json& params, const char* key = "id") {
    const Json& v = params[key];
    if (!v.isNumber()) fail(protocol::kInvalidParams, std::string("missing numeric '") + key + "'");
    return v.asUInt();
}

std::vector<ObjectId> idList(const Json& params, const char* key) {
    std::vector<ObjectId> ids;
    const Json& v = params[key];
    if (!v.isArray()) fail(protocol::kInvalidParams, std::string("'") + key + "' must be an array of ids");
    for (const auto& e : v.asArray()) ids.push_back(e.asUInt());
    return ids;
}

Json idsJson(const std::vector<ObjectId>& ids) {
    Json a = Json::array();
    for (ObjectId id : ids) a.push_back(id);
    return a;
}

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

uint64_t residentMemoryBytes() {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
        KERN_SUCCESS)
        return info.resident_size;
    return 0;
#elif defined(__linux__)
    std::ifstream f("/proc/self/statm");
    uint64_t size = 0, resident = 0;
    f >> size >> resident;
    return resident * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
#else
    return 0;
#endif
}

EngineSession::EngineSession(std::vector<fs::path> extensionDirs, LogFn log, fs::path extensionStateFile)
    : extensions_([log](int level, const std::string& id, const std::string& msg) {
          if (log) log(level, id, msg);
      }),
      log_(std::move(log)),
      extensionStateFile_(std::move(extensionStateFile)) {
    std::error_code ec;
    if (!extensionStateFile_.empty() && fs::is_regular_file(extensionStateFile_, ec)) {
        try {  // disabled extensions must never be loaded, not even briefly: apply before the first load
            std::set<std::string> off;
            const Json st = readJsonFile(extensionStateFile_);  // kept alive while iterating
            for (const auto& v : st["disabled"].asArray()) off.insert(v.asString());
            extensions_.setDisabled(std::move(off));
        } catch (const std::exception&) {
            if (log_) log_(2, "engine", "ignoring unreadable extension state " + extensionStateFile_.string());
        }
    }
    extensions_.loadFrom(extensionDirs);
}

Json EngineSession::handle(const Json& request, std::vector<Json>& events) {
    const auto t0 = std::chrono::steady_clock::now();
    Json id = request["id"];
    std::string method;
    Json response;
    try {
        if (!request.isObject() || !id.isNumber() || !request["method"].isString())
            fail(protocol::kInvalidRequest, "request needs numeric id and string method");
        method = request["method"].asString();
        const Json& params = request.contains("params") ? request["params"] : Json::object();
        if (!params.isObject()) fail(protocol::kInvalidParams, "params must be an object");
        response = protocol::makeResult(id, dispatch(method, params, events));
    } catch (const ProtocolError& e) {
        response = protocol::makeError(id, e.code, e.what());
    } catch (const ProjectError& e) {
        response = protocol::makeError(id, protocol::kIoError, e.what());
    } catch (const JsonError& e) {
        response = protocol::makeError(id, protocol::kInvalidParams, e.what());
    } catch (const std::invalid_argument& e) {
        response = protocol::makeError(id, protocol::kInvalidParams, e.what());
    } catch (const std::exception& e) {
        response = protocol::makeError(id, protocol::kInternalError, e.what());
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    metrics_.requests++;
    if (response.contains("error")) metrics_.errors++;
    metrics_.totalRequestUs += us;
    metrics_.maxRequestUs = std::max(metrics_.maxRequestUs, us);
    auto& pm = metrics_.perMethod[method.empty() ? "<invalid>" : method];
    pm.first++;
    pm.second += us;
    return response;
}

void EngineSession::requireProject() const {
    if (!project_) fail(protocol::kNoProject, "no project is open");
}

Json EngineSession::stateJson() const {
    Json s = Json::object();
    s["projectOpen"] = project_.has_value();
    s["path"] = project_ ? project_->root.string() : std::string();
    s["name"] = project_ ? project_->name : std::string();
    s["dirty"] = project_ ? undo_.isDirty() : false;
    s["revision"] = scene_.revision();
    s["canUndo"] = undo_.canUndo();
    s["canRedo"] = undo_.canRedo();
    s["undoLabel"] = undo_.undoLabel().value_or("");
    s["redoLabel"] = undo_.redoLabel().value_or("");
    s["selection"] = idsJson(selection_);
    return s;
}

Json EngineSession::sceneJson() const {
    Json j = Json::object();
    j["name"] = scene_.name;
    j["revision"] = scene_.revision();
    j["environment"] = scene_.environment.toJson();
    Json objs = Json::array();
    for (const auto& [id, o] : scene_.objects()) objs.push_back(o.toJson());
    j["objects"] = objs;
    Json rs = Json::array();
    for (const auto& r : scene_.recipes) rs.push_back(r.toJson());
    j["recipes"] = rs;
    return j;
}

void EngineSession::emitChanged(const std::vector<ObjectId>& ids, std::vector<Json>& events,
                                const std::string& reason) {
    Json upserted = Json::array(), removed = Json::array();
    std::set<ObjectId> unique(ids.begin(), ids.end());
    for (ObjectId id : unique) {
        if (const SceneObject* o = scene_.find(id)) upserted.push_back(o->toJson());
        else removed.push_back(id);
    }
    // Selection must never reference deleted objects.
    size_t before = selection_.size();
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(),
                                    [&](ObjectId id) { return !scene_.contains(id); }),
                     selection_.end());
    Json data = Json::object();
    data["revision"] = scene_.revision();
    data["reason"] = reason;
    data["upserted"] = upserted;
    data["removed"] = removed;
    Json rs = Json::array();
    for (const auto& r : scene_.recipes) rs.push_back(r.toJson());
    data["recipes"] = rs;
    data["state"] = stateJson();
    events.push_back(protocol::makeEvent("scene.changed", data));
    if (selection_.size() != before) {
        Json sel = Json::object();
        sel["ids"] = idsJson(selection_);
        events.push_back(protocol::makeEvent("selection.changed", sel));
    }
}

void EngineSession::commit(SceneEdit edit, std::vector<Json>& events, const std::string& reason) {
    if (edit.empty()) return;
    auto ids = edit.touchedIds();
    undo_.push(scene_, std::move(edit));
    emitChanged(ids, events, reason);
}

namespace {
constexpr size_t kMaxBookmarks = 32;
// A camera bookmark: {"name": "...", "camera": {"target": [x, y, z], "yaw": n, "pitch": n, "distance": n > 0}}.
bool validBookmark(const Json& e) {
    if (!e.isObject() || !e["name"].isString()) return false;
    const std::string& n = e["name"].asString();
    if (n.empty() || n.size() > 64) return false;
    const Json& c = e["camera"];
    if (!c.isObject() || !c["yaw"].isNumber() || !c["pitch"].isNumber() || !c["distance"].isNumber() || !(c["distance"].asNumber() > 0))
        return false;
    const Json& t = c["target"];
    if (!t.isArray() || t.asArray().size() != 3) return false;
    for (const auto& v : t.asArray())
        if (!v.isNumber() || !std::isfinite(v.asNumber())) return false;
    return std::isfinite(c["yaw"].asNumber()) && std::isfinite(c["pitch"].asNumber()) && std::isfinite(c["distance"].asNumber());
}
}  // namespace

void EngineSession::writeEditorState() {
    if (!project_) return;
    Json j = editorState_;
    j["format"] = "titan.editor-state";
    j["selection"] = idsJson(selection_);
    try {
        writeFileAtomic(project_->editorStateFile(), j.dump(2) + "\n");
    } catch (const std::exception& e) {
        if (log_) log_(TITAN_LOG_WARNING, "engine", std::string("editor state not written: ") + e.what());
    }
}

void EngineSession::closeProject() {
    writeEditorState();
    project_.reset();
    scene_ = Scene();
    undo_.clear();
    selection_.clear();
    editorState_ = Json::object();
    proposal_.reset();
}

fs::path EngineSession::autosaveFile() const { return project_->root / "Saved" / "Autosave.json"; }

bool EngineSession::writeAutosave() {
    if (!project_) return false;
    Json j = Json::object();
    j["format"] = 1;
    j["savedAtMs"] = double(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count());
    j["revision"] = double(scene_.revision());
    j["scene"] = scene_.toJson();
    try {
        fs::create_directories(autosaveFile().parent_path());
        writeFileAtomic(autosaveFile(), j.dump() + "\n");
    } catch (const std::exception& e) {
        if (log_) log_(1, "engine", std::string("autosave failed: ") + e.what());
        return false;
    }
    autosavedRevision_ = scene_.revision();
    lastAutosave_ = std::chrono::steady_clock::now();
    return true;
}

void EngineSession::removeAutosave() {
    std::error_code ec;
    if (project_) fs::remove(autosaveFile(), ec);
    autosavedRevision_ = scene_.revision();
}

std::optional<Scene> EngineSession::readRecovery() const {
    std::error_code ec;
    if (!project_ || !fs::is_regular_file(autosaveFile(), ec)) return std::nullopt;
    try {
        const Json j = readJsonFile(autosaveFile());
        if (j.getNumber("format", 0) != 1) return std::nullopt;
        Scene s = Scene::fromJson(j["scene"]);  // validated like any scene file
        const Json a = s.toJson(), b = scene_.toJson();
        if (a["objects"] == b["objects"] && a["recipes"] == b["recipes"])
            return std::nullopt;  // nothing to recover
        return s;
    } catch (const std::exception&) {
        return std::nullopt;  // a damaged recovery point is ignored, never loaded
    }
}

void EngineSession::tick() {
    if (!project_ || !undo_.isDirty() || scene_.revision() == autosavedRevision_) return;
    if (std::chrono::steady_clock::now() - lastAutosave_ < autosaveInterval) return;
    writeAutosave();
}

Json EngineSession::openProject(const fs::path& path, std::vector<Json>& events) {
    const auto t0 = std::chrono::steady_clock::now();
    ProjectInfo info = readProject(path);
    Scene scene = loadScene(info);  // load fully before touching current state
    if (project_) closeProject();
    project_ = info;
    scene_ = std::move(scene);
    undo_.clear();
    selection_.clear();
    editorState_ = Json::object();
    std::error_code ec;
    if (fs::is_regular_file(info.editorStateFile(), ec)) {
        try {
            Json st = readJsonFile(info.editorStateFile());
            if (st.isObject()) {
                if (st.contains("camera")) editorState_["camera"] = st["camera"];
                if (const Json* b = st.find("bookmarks")) {  // a hand-edited file keeps only its valid bookmarks
                    Json kept = Json::array();
                    if (b->isArray())
                        for (const auto& e : b->asArray())
                            if (validBookmark(e) && kept.asArray().size() < kMaxBookmarks) kept.push_back(e);
                    editorState_["bookmarks"] = kept;
                }
                if (st["selection"].isArray())
                    for (const auto& v : st["selection"].asArray())
                        if (v.isNumber() && scene_.contains(v.asUInt())) selection_.push_back(v.asUInt());
            }
        } catch (const std::exception&) {
            // Corrupt per-user state is not fatal; content is unaffected.
        }
    }
    metrics_.lastOpenMs = msSince(t0);
    Json r = Json::object();
    r["project"] = info.toJson();
    r["project"]["path"] = info.root.string();
    r["scene"] = sceneJson();
    r["editor"] = editorState_;
    r["state"] = stateJson();
    r["openMs"] = metrics_.lastOpenMs;
    autosavedRevision_ = scene_.revision();
    if (auto rec = readRecovery()) {  // unsaved work from a session that did not close cleanly
        Json rec_ = Json::object();
        rec_["available"] = true;
        rec_["objects"] = double(rec->objects().size());
        rec_["savedAtMs"] = readJsonFile(autosaveFile()).getNumber("savedAtMs", 0);
        r["recovery"] = rec_;
    }
    events.push_back(protocol::makeEvent("project.opened", r["state"]));
    return r;
}

Json EngineSession::objectUpdate(const Json& params, std::vector<Json>& events) {
    requireProject();
    ObjectId id = requireId(params);
    const SceneObject* cur = scene_.find(id);
    if (!cur) fail(protocol::kNotFound, "object " + std::to_string(id) + " not found");
    const Json& set = params["set"];
    if (!set.isObject() || set.size() == 0) fail(protocol::kInvalidParams, "'set' must be a non-empty object");
    std::string what;
    SceneObject after = applySet(*cur, set, what);
    SceneEdit edit(what + " " + cur->name, params.getString("coalesce"));
    edit.recordUpdate(scene_, after);
    commit(std::move(edit), events, "update");
    Json r = Json::object();
    r["object"] = scene_.find(id)->toJson();
    r["revision"] = scene_.revision();
    return r;
}

// Applies an object.update "set" to a copy of `cur`; throws a protocol error for invalid input.
SceneObject EngineSession::applySet(const SceneObject& cur, const Json& set, std::string& what) const {
    SceneObject after = cur;
    for (const auto& [key, value] : set.asObject()) {
        if (key == "name") {
            if (!value.isString() || value.asString().empty()) fail(protocol::kInvalidParams, "name must be non-empty");
            after.name = value.asString();
            what = "Rename";
        } else if (key == "transform") {
            if (cur.locked) fail(protocol::kInvalidParams, "'" + cur.name + "' is locked; unlock it to move it");
            after.transform = transformFromJson(value, after.transform);
            if (after.transform.scale.x == 0 || after.transform.scale.y == 0 || after.transform.scale.z == 0)
                fail(protocol::kInvalidParams, "scale components must be non-zero");
            if (what.empty()) what = "Transform";
        } else if (key == "color") {
            after.color = colorFromJson(value);
            if (what.empty()) what = "Color";
        } else if (key == "visible") {
            after.visible = value.asBool();
            if (what.empty()) what = after.visible ? "Show" : "Hide";
        } else if (key == "locked") {
            after.locked = value.asBool();
            if (what.empty()) what = after.locked ? "Lock" : "Unlock";
        } else if (key == "intensity") {
            after.intensity = static_cast<float>(std::max(0.0, value.asNumber()));
            if (what.empty()) what = "Intensity";
        } else {
            fail(protocol::kInvalidParams, "property '" + key + "' is not editable");
        }
    }
    return after;
}

Json EngineSession::objectUpdateMany(const std::string& method, const Json& params, std::vector<Json>& events) {
    // Multi-object editing (capability edit.multi): every object is validated before anything changes, and the
    // batch is ONE undo step (coalescable for drags/nudges).
    requireProject();
    std::vector<ObjectId> ids;
    for (ObjectId id : idList(params, "ids")) {
        if (!scene_.contains(id)) fail(protocol::kNotFound, "object " + std::to_string(id) + " not found");
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    }
    if (ids.empty()) fail(protocol::kInvalidParams, "no objects");
    std::vector<SceneObject> afters;
    std::string what = "Edit";
    if (method == "object.updateMany") {
        const Json& set = params["set"];
        if (!set.isObject() || set.size() == 0) fail(protocol::kInvalidParams, "'set' must be a non-empty object");
        if (set.contains("name")) fail(protocol::kInvalidParams, "objects are renamed one at a time");
        for (ObjectId id : ids) afters.push_back(applySet(*scene_.find(id), set, what));
    } else {
        // object.transformMany: position/rotation/scale are [x, y, z] with null for "keep each object's value".
        // mode "set" assigns the given components; mode "offset" adds them (position, rotation) or multiplies
        // them (scale).
        const std::string mode = params.getString("mode", "set");
        if (mode != "set" && mode != "offset") fail(protocol::kInvalidParams, "mode must be 'set' or 'offset'");
        auto parts = [&](const char* key) {
            std::array<std::optional<float>, 3> v{};
            const Json& a = params[key];
            if (a.isNull()) return v;
            if (!a.isArray() || a.size() != 3) fail(protocol::kInvalidParams, std::string(key) + " must be [x, y, z] (null keeps a value)");
            for (size_t i = 0; i < 3; ++i) {
                const Json& c = a.at(i);
                if (c.isNull()) continue;
                if (!c.isNumber() || !std::isfinite(c.asNumber())) fail(protocol::kInvalidParams, std::string(key) + " components must be finite numbers");
                v[i] = float(c.asNumber());
            }
            return v;
        };
        const auto pos = parts("position"), rot = parts("rotation"), scl = parts("scale");
        auto applyTo = [&](Vec3& t, const std::array<std::optional<float>, 3>& v, bool multiply) {
            float* c[3] = {&t.x, &t.y, &t.z};
            for (int i = 0; i < 3; ++i)
                if (v[i]) *c[i] = mode == "set" ? *v[i] : (multiply ? *c[i] * *v[i] : *c[i] + *v[i]);
        };
        for (ObjectId id : ids) {
            SceneObject o = *scene_.find(id);
            if (o.locked) fail(protocol::kInvalidParams, "'" + o.name + "' is locked; nothing was changed");
            applyTo(o.transform.position, pos, false);
            applyTo(o.transform.rotation, rot, false);
            applyTo(o.transform.scale, scl, true);
            const Vec3& s = o.transform.scale;
            if (s.x == 0 || s.y == 0 || s.z == 0)
                fail(protocol::kInvalidParams, "scale of '" + o.name + "' would become zero; nothing was changed");
            afters.push_back(o);
        }
        what = mode == "set" ? "Transform" : "Offset";
    }
    SceneEdit edit(what + " " + std::to_string(ids.size()) + " objects", params.getString("coalesce"));
    for (const auto& o : afters) edit.recordUpdate(scene_, o);
    commit(std::move(edit), events, "update");
    Json r = Json::object();
    r["ids"] = idsJson(ids);
    r["revision"] = scene_.revision();
    return r;
}

Json EngineSession::objectCreate(const Json& params, std::vector<Json>& events) {
    requireProject();
    Json spec = params;
    spec["id"] = 1;  // placeholder so fromJson validates the rest
    SceneObject obj = SceneObject::fromJson(spec);
    if (obj.parent != kNoObject && !scene_.contains(obj.parent)) fail(protocol::kNotFound, "parent not found");
    obj.id = scene_.allocateId();
    if (!params.contains("name")) obj.name = std::string(toString(obj.shape)) + " " + std::to_string(obj.id);
    obj.generator.reset();  // provenance tags are engine-assigned only
    SceneEdit edit("Create " + obj.name);
    edit.recordCreate(obj);
    commit(std::move(edit), events, "create");
    Json r = Json::object();
    r["id"] = obj.id;
    r["object"] = obj.toJson();
    return r;
}

Json EngineSession::objectDelete(const Json& params, std::vector<Json>& events) {
    requireProject();
    auto ids = scene_.withDescendants(idList(params, "ids"));
    if (ids.empty()) fail(protocol::kNotFound, "no such objects");
    SceneEdit edit(ids.size() == 1 ? "Delete " + scene_.find(ids[0])->name
                                   : "Delete " + std::to_string(ids.size()) + " objects");
    for (ObjectId id : ids) edit.recordDelete(scene_, id);
    commit(std::move(edit), events, "delete");
    Json r = Json::object();
    r["deleted"] = idsJson(ids);
    return r;
}

Json EngineSession::worldPreview(const Json& params) {
    requireProject();
    std::string ext = params.getString("extension");
    if (!extensions_.isLoaded(ext)) fail(protocol::kExtensionError, "extension not loaded: " + ext);
    if (!extensions_.hasCapability(ext, "scene.propose"))
        fail(protocol::kExtensionError, "extension lacks capability scene.propose");
    const Json& recipe = params["recipe"];
    if (!recipe.isObject()) fail(protocol::kInvalidParams, "'recipe' must be an object");
    std::string recipeId = params.getString("recipeId");
    if (!recipeId.empty() && !scene_.findRecipe(recipeId)) fail(protocol::kNotFound, "recipe not found: " + recipeId);

    // Keep-out discs from existing visible geometry (except the ground and this recipe's output).
    Json avoid = Json::array();
    for (const auto& [id, o] : scene_.objects()) {
        if (!o.visible || o.shape == Shape::None || o.shape == Shape::Plane) continue;
        if (!recipeId.empty() && o.generator && o.generator->recipeId == recipeId) continue;
        Mat4 w = scene_.worldMatrix(id);
        Vec3 c = w.transformPoint({0, 0, 0});
        float rx = length(w.transformDirection({0.5f, 0, 0})), rz = length(w.transformDirection({0, 0, 0.5f}));
        avoid.push_back(Json(Json::Array{c.x, c.z, std::max(rx, rz)}));
    }
    Json p = Json::object();
    p["recipe"] = recipe;
    p["avoid"] = avoid;
    Json out;
    try {
        out = extensions_.invoke(ext, "generate", p);
    } catch (const std::exception& e) {
        fail(protocol::kExtensionError, e.what());
    }
    Proposal prop;
    prop.id = "proposal-" + std::to_string(++proposalCounter_);
    prop.extension = ext;
    prop.recipeId = recipeId;
    prop.recipe = recipe;
    Json previews = Json::array();
    for (const auto& oj : out["objects"].asArray()) {
        Json spec = oj;
        spec["id"] = 1;
        SceneObject o = SceneObject::fromJson(spec);  // validates extension output
        o.id = kNoObject;
        o.parent = kNoObject;
        o.generator.reset();
        prop.objects.push_back(o);
        Json pj = o.toJson();
        pj.erase("id");
        previews.push_back(pj);
    }
    Json r = Json::object();
    r["proposalId"] = prop.id;
    r["objects"] = previews;
    r["produced"] = out["produced"];
    r["requested"] = out["requested"];
    if (out.contains("warning")) r["warning"] = out["warning"];
    Json replaces = Json::array();
    if (!recipeId.empty())
        for (const auto& [id, o] : scene_.objects())
            if (o.generator && o.generator->recipeId == recipeId && o.type != ObjectType::Group) replaces.push_back(id);
    r["replaces"] = replaces;
    proposal_ = std::move(prop);
    return r;
}

Json EngineSession::worldApprove(const Json& params, std::vector<Json>& events) {
    requireProject();
    if (!proposal_ || proposal_->id != params.getString("proposalId"))
        fail(protocol::kConflict, "no matching pending proposal (it may have been superseded)");
    Proposal prop = std::move(*proposal_);
    proposal_.reset();

    std::string recipeId = prop.recipeId;
    if (recipeId.empty()) {
        int n = 1;
        while (scene_.findRecipe("scatter-" + std::to_string(n))) ++n;
        recipeId = "scatter-" + std::to_string(n);
    }
    SceneEdit edit((prop.recipeId.empty() ? "Generate " : "Regenerate ") + recipeId + " (" +
                   std::to_string(prop.objects.size()) + " objects)");
    // Reuse the recipe's group when regenerating; delete only that recipe's previous output.
    ObjectId group = kNoObject;
    for (const auto& [id, o] : scene_.objects()) {
        if (!o.generator || o.generator->recipeId != recipeId) continue;
        if (o.type == ObjectType::Group && group == kNoObject) group = id;
        else edit.recordDelete(scene_, id);
    }
    GeneratorTag tag{prop.extension, recipeId};
    if (group == kNoObject) {
        SceneObject g;
        g.id = scene_.allocateId();
        g.name = "World Builder · " + recipeId;
        g.type = ObjectType::Group;
        g.shape = Shape::None;
        g.generator = tag;
        edit.recordCreate(g);
        group = g.id;
    }
    std::vector<ObjectId> created;
    for (SceneObject o : prop.objects) {
        o.id = scene_.allocateId();
        o.parent = group;
        o.generator = tag;
        edit.recordCreate(o);
        created.push_back(o.id);
    }
    edit.recordRecipe(scene_, Recipe{recipeId, prop.extension, prop.recipe}, recipeId);
    commit(std::move(edit), events, "worldbuilder");
    Json r = Json::object();
    r["recipeId"] = recipeId;
    r["groupId"] = group;
    r["created"] = idsJson(created);
    return r;
}

const std::vector<std::string>& EngineSession::supportedCapabilities() {
    static const std::vector<std::string> caps = {"project", "scene", "selection", "undo", "pick", "extensions", "worldbuilder.preview", "assets", "prefabs", "edit.duplicate", "edit.group", "recovery", "templates", "extensions.manage", "edit.multi", "edit.reparent", "edit.lock", "camera.bookmarks", "edit.clipboard", "edit.history"};
    return caps;
}

Json EngineSession::dispatch(const std::string& method, const Json& params, std::vector<Json>& events) {
    // Capability contract: a method of a capability this backend does not advertise is refused explicitly.
    if (const CapabilitySpec* cap = capabilityForMethod(method)) {
        const auto& mine = supportedCapabilities();
        if (std::find(mine.begin(), mine.end(), cap->name) == mine.end())
            fail(protocol::kUnsupported, "'" + method + "' needs capability '" + cap->name + "', which " +
                                             "titan-reference-host does not provide");
    }
    if (method == "session.hello") {
        const Json& p = params["protocol"];
        int cmin = static_cast<int>(p.getNumber("min", 1)), cmax = static_cast<int>(p.getNumber("max", 1));
        int chosen = std::min(cmax, kProtocolVersion);
        if (chosen < std::max(cmin, kProtocolMin))
            fail(protocol::kVersionMismatch, "no common protocol version (server supports " +
                                                 std::to_string(kProtocolMin) + "-" + std::to_string(kProtocolVersion) + ")");
        Json r = Json::object();
        r["protocol"] = chosen;
        r["protocolName"] = kProtocolName;
        Json engine = Json::object();
        engine["product"] = kProductName;
        engine["version"] = kEngineVersion;
        engine["backend"] = "titan-reference-host";
        engine["platform"] = currentPlatform();
        r["engine"] = engine;
        Json caps = Json::array(), versions = Json::object();
        for (const std::string& c : supportedCapabilities()) {
            caps.push_back(c);
            versions[c] = findCapability(c) ? findCapability(c)->version : 1;
        }
        r["capabilities"] = caps;
        r["capabilityVersions"] = versions;  // contract: titan/capabilities.h
        r["state"] = stateJson();
        return r;
    }
    if (method == "engine.status") {
        Json r = stateJson();
        r["uptimeMs"] = msSince(started_);
        r["objectCount"] = scene_.size();
        r["rssBytes"] = residentMemoryBytes();
        Json m = Json::object();
        m["requests"] = metrics_.requests;
        m["errors"] = metrics_.errors;
        m["avgRequestUs"] = metrics_.requests ? metrics_.totalRequestUs / double(metrics_.requests) : 0.0;
        m["maxRequestUs"] = metrics_.maxRequestUs;
        m["lastOpenMs"] = metrics_.lastOpenMs;
        m["lastSaveMs"] = metrics_.lastSaveMs;
        r["metrics"] = m;
        return r;
    }
    if (method == "project.create") {
        std::string path = params.getString("path"), name = params.getString("name");
        if (path.empty() || name.empty()) fail(protocol::kInvalidParams, "path and name are required");
        if (project_ && undo_.isDirty() && !params.getBool("force", false))
            fail(protocol::kConflict, "current project has unsaved changes");
        const std::string tname = params.getString("template");
        if (tname.empty()) {
            createProject(path, name);
        } else {
            std::optional<SceneTemplate> chosen;
            for (auto& t : listTemplates(templatesDir))
                if (t.name == tname) chosen = std::move(t);
            if (!chosen) fail(protocol::kNotFound, "unknown template '" + tname + "'");
            chosen->scene.name = "Main";
            createProject(path, name, &chosen->scene);
        }
        return openProject(path, events);
    }
    if (method == "project.open") {
        std::string path = params.getString("path");
        if (path.empty()) fail(protocol::kInvalidParams, "path is required");
        if (project_ && undo_.isDirty() && !params.getBool("force", false))
            fail(protocol::kConflict, "current project has unsaved changes");
        return openProject(path, events);
    }
    if (method == "project.save") {
        requireProject();
        const auto t0 = std::chrono::steady_clock::now();
        saveScene(*project_, scene_);
        writeEditorState();
        undo_.markSaved();
        removeAutosave();  // the saved scene supersedes the recovery point
        metrics_.lastSaveMs = msSince(t0);
        Json r = Json::object();
        r["path"] = project_->sceneFile().string();
        r["saveMs"] = metrics_.lastSaveMs;
        r["state"] = stateJson();
        events.push_back(protocol::makeEvent("project.state", r["state"]));
        return r;
    }
    if (method == "project.close") {
        requireProject();
        if (undo_.isDirty() && !params.getBool("force", false))
            fail(protocol::kConflict, "project has unsaved changes (pass force: true to discard)");
        removeAutosave();  // a deliberate close (saved or discarded) leaves nothing to recover
        closeProject();
        events.push_back(protocol::makeEvent("project.closed", stateJson()));
        return stateJson();
    }
    if (method == "scene.query") {
        requireProject();
        return sceneJson();
    }
    if (method == "object.get") {
        requireProject();
        const SceneObject* o = scene_.find(requireId(params));
        if (!o) fail(protocol::kNotFound, "object not found");
        Json r = o->toJson();
        Mat4 w = scene_.worldMatrix(o->id);
        Json world = Json::array();
        for (float f : w.m) world.push_back(f);
        r["worldMatrix"] = world;
        return r;
    }
    if (method == "object.update") return objectUpdate(params, events);
    if (method == "object.updateMany" || method == "object.transformMany") return objectUpdateMany(method, params, events);
    if (method == "object.create") return objectCreate(params, events);
    if (method == "object.delete") return objectDelete(params, events);
    if (method == "selection.set") {
        requireProject();
        std::vector<ObjectId> ids;
        for (ObjectId id : idList(params, "ids")) {
            if (!scene_.contains(id)) fail(protocol::kNotFound, "object " + std::to_string(id) + " not found");
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        }
        selection_ = ids;
        Json sel = Json::object();
        sel["ids"] = idsJson(selection_);
        events.push_back(protocol::makeEvent("selection.changed", sel));
        return sel;
    }
    if (method == "scene.pick") {
        requireProject();
        Ray ray{vec3FromJson(params["origin"]), vec3FromJson(params["direction"])};
        if (length(ray.direction) == 0) fail(protocol::kInvalidParams, "direction must be non-zero");
        Json r = Json::object();
        if (auto hit = pick(scene_, ray)) {
            r["id"] = hit->id;
            r["distance"] = hit->distance;
        } else {
            r["id"] = nullptr;
        }
        return r;
    }
    if (method == "edit.history") {
        // The existing undo stack, read-only: labels oldest first, the current position (number of applied
        // edits) and the position of the last save (-1 when that state can no longer be reached).
        requireProject();
        Json entries = Json::array();
        for (const auto& l : undo_.labels()) entries.push_back(l);
        Json r = Json::object();
        r["entries"] = entries;
        r["position"] = double(undo_.depth());
        r["saved"] = undo_.savedPosition() ? double(*undo_.savedPosition()) : -1.0;
        return r;
    }
    if (method == "edit.goto") {
        // Multi-step navigation: undoes or redoes one edit at a time until the history is at `position`.
        requireProject();
        const Json& pv = params["position"];
        if (!pv.isNumber() || pv.asNumber() < 0 || pv.asNumber() > double(undo_.size()) || pv.asNumber() != std::floor(pv.asNumber()))
            fail(protocol::kInvalidParams, "position must be an integer between 0 and " + std::to_string(undo_.size()));
        const size_t target = size_t(pv.asNumber());
        std::set<ObjectId> touched;
        size_t steps = 0;
        while (undo_.depth() != target) {
            auto ids = undo_.depth() > target ? undo_.undo(scene_) : undo_.redo(scene_);
            if (!ids) break;
            touched.insert(ids->begin(), ids->end());
            ++steps;
        }
        if (steps) emitChanged(std::vector<ObjectId>(touched.begin(), touched.end()), events, "history");
        Json r = Json::object();
        r["steps"] = double(steps);
        r["position"] = double(undo_.depth());
        r["state"] = stateJson();
        return r;
    }
    if (method == "edit.undo" || method == "edit.redo") {
        requireProject();
        const bool isUndo = method == "edit.undo";
        auto label = isUndo ? undo_.undoLabel() : undo_.redoLabel();
        auto ids = isUndo ? undo_.undo(scene_) : undo_.redo(scene_);
        Json r = Json::object();
        r["applied"] = ids.has_value();
        r["label"] = label.value_or("");
        if (ids) emitChanged(*ids, events, isUndo ? "undo" : "redo");
        r["state"] = stateJson();
        return r;
    }
    if (method == "editor.setState") {
        requireProject();
        if (const Json* b = params.find("bookmarks")) {  // capability camera.bookmarks: replaces the whole list
            if (!b->isArray() || b->asArray().size() > kMaxBookmarks)
                fail(protocol::kInvalidParams, "bookmarks must be an array of at most " + std::to_string(kMaxBookmarks));
            for (const auto& e : b->asArray())
                if (!validBookmark(e)) fail(protocol::kInvalidParams, "each bookmark needs a name (1-64 chars) and a camera {target:[x,y,z], yaw, pitch, distance > 0}");
            editorState_["bookmarks"] = *b;
        }
        if (params.contains("camera")) editorState_["camera"] = params["camera"];
        writeEditorState();
        return Json::object();
    }
    if (method == "extensions.list") {
        Json r = Json::object();
        r["extensions"] = extensions_.describeAll();
        r["platform"] = currentPlatform();
        return r;
    }
    if (method == "extensions.setEnabled" || method == "extensions.reload") {
        // Both rescan the extension directories; any open World Builder proposal is dropped (it may come from an
        // extension that is about to be unloaded).
        if (method == "extensions.setEnabled") {
            if (extensionStateFile_.empty()) fail(protocol::kInvalidParams, "no extension state file (engine host --extension-state)");
            const std::string id = params.getString("id");
            if (!extensions_.isKnown(id)) fail(protocol::kNotFound, "extension '" + id + "' is not installed");
            std::set<std::string> off = extensions_.disabled();
            if (params.getBool("enabled", true)) off.erase(id);
            else off.insert(id);
            Json st = Json::object();
            st["format"] = 1;
            Json arr = Json::array();
            for (const auto& d : off) arr.push_back(d);
            st["disabled"] = arr;
            fs::create_directories(extensionStateFile_.parent_path());
            writeFileAtomic(extensionStateFile_, st.dump(1) + "\n");
            extensions_.setDisabled(std::move(off));
        }
        proposal_.reset();
        extensions_.reload();
        Json r = Json::object();
        r["extensions"] = extensions_.describeAll();
        r["platform"] = currentPlatform();
        return r;
    }
    if (method == "extensions.invoke") {
        // Read-only commands only; scene changes must go through worldbuilder.* approval.
        std::string ext = params.getString("extension"), cmd = params.getString("command");
        if (cmd != "describe") fail(protocol::kInvalidParams, "only 'describe' may be invoked directly");
        try {
            return extensions_.invoke(ext, cmd, params["params"].isObject() ? params["params"] : Json::object());
        } catch (const std::exception& e) {
            fail(protocol::kExtensionError, e.what());
        }
    }
    if (method == "worldbuilder.preview") return worldPreview(params);
    if (method == "worldbuilder.approve") return worldApprove(params, events);
    if (method == "worldbuilder.reject") {
        bool had = proposal_ && proposal_->id == params.getString("proposalId");
        if (had) proposal_.reset();
        Json r = Json::object();
        r["rejected"] = had;
        return r;
    }
    if (method == "assets.list" || method == "assets.refresh") {
        // Asset pipeline (docs/ASSET_PIPELINE.md). The registry lives in <project>/Library; a refresh imports what
        // changed under <project>/Assets and is synchronous (bounded by the project size; no file watcher yet).
        requireProject();
        assets::AssetDatabase db(project_->root);
        Json r = Json::object();
        if (method == "assets.refresh") {
            const assets::RefreshStats st = db.refresh();
            Json s = Json::object();
            s["scanned"] = st.scanned;
            s["hashed"] = st.hashed;
            s["imported"] = st.imported;
            s["reused"] = st.reused;
            s["failed"] = st.failed;
            s["missing"] = st.missing;
            s["renamed"] = st.renamed;
            r["stats"] = s;
            events.push_back(protocol::makeEvent("assets.changed", s));
        }
        Json list = Json::array();
        for (const auto& a : db.records()) {
            Json j = Json::object();
            j["id"] = a.id;
            j["path"] = a.path;
            j["importer"] = a.importer;
            j["status"] = assets::statusName(a.status);
            if (!a.error.empty()) j["error"] = a.error;
            j["sourceSize"] = double(a.sourceSize);
            j["metadata"] = a.metadata.isNull() ? Json::object() : a.metadata;
            Json deps = Json::array();
            for (const auto& d : a.dependencies) deps.push_back(d);
            j["dependencies"] = deps;
            list.push_back(j);
        }
        r["assets"] = list;
        return r;
    }
    if (method == "object.copy") {
        requireProject();
        Clipboard clip;
        try {
            clip = Clipboard::copy(scene_, idList(params, "ids"), project_->root.lexically_normal().string());
        } catch (const std::invalid_argument& e) {
            fail(std::string(e.what()).find("not found") != std::string::npos ? protocol::kNotFound : protocol::kInvalidParams, e.what());
        }
        Json r = Json::object();
        r["clipboard"] = clip.toJson();
        r["count"] = double(clip.objects.size());
        return r;
    }
    if (method == "object.duplicate" || method == "object.group" || method == "object.ungroup" || method == "object.reparent" || method == "object.paste") {
        requireProject();
        auto select = [&](const std::vector<ObjectId>& ids) {
            selection_ = ids;
            Json sel = Json::object();
            sel["ids"] = idsJson(selection_);
            events.push_back(protocol::makeEvent("selection.changed", sel));
        };
        // Top-level members of a set: drop ids whose ancestor is also in the set (it brings them along).
        auto roots = [&](const std::vector<ObjectId>& in) {
            std::set<ObjectId> all;
            for (ObjectId id : in) {
                if (!scene_.contains(id)) fail(protocol::kNotFound, "object " + std::to_string(id) + " not found");
                all.insert(id);
            }
            std::vector<ObjectId> out;
            for (ObjectId id : in) {
                bool nested = false;
                for (ObjectId p = scene_.find(id)->parent; p != kNoObject && !nested; p = scene_.find(p)->parent)
                    nested = all.count(p) != 0;
                if (!nested && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
            }
            return out;
        };
        if (method == "object.paste") {
            // Pastes a clipboard (object.copy, possibly from another project) as one undo step: fresh ids,
            // parent links remapped inside the paste, roots under `parent` (0 = scene root) at their copied
            // world placement. Selects the pasted roots.
            const ObjectId parent = ObjectId(params.getNumber("parent", 0));
            if (parent != kNoObject && !scene_.contains(parent)) fail(protocol::kNotFound, "parent not found");
            std::vector<SceneObject> objs;
            try {
                const Clipboard clip = Clipboard::fromJson(params["clipboard"]);
                objs = clip.paste(scene_, parent, clip.sourceProject == project_->root.lexically_normal().string());
            } catch (const std::invalid_argument& e) {
                fail(protocol::kInvalidParams, e.what());
            } catch (const std::exception& e) {
                fail(protocol::kInvalidParams, std::string("clipboard is malformed: ") + e.what());
            }
            std::vector<ObjectId> roots, ids;
            for (const auto& o : objs) {
                ids.push_back(o.id);
                if (o.parent == parent) roots.push_back(o.id);
            }
            SceneEdit edit("Paste " + (roots.size() == 1 && objs.size() == 1 ? objs[0].name : std::to_string(objs.size()) + " objects"));
            for (const auto& o : objs) edit.recordCreate(o);
            commit(std::move(edit), events, "paste");
            select(roots);
            Json r = Json::object();
            r["ids"] = idsJson(ids);
            r["roots"] = idsJson(roots);
            return r;
        }
        if (method == "object.reparent") {
            // Moves the top-level members of `ids` (descendants come along) under `parent` (0 = scene root) as
            // one undo step. keepWorld (default true) rewrites each local transform so nothing moves on screen;
            // the whole batch is refused if that is impossible (shear) or if it would create a cycle.
            const auto top = roots(idList(params, "ids"));
            if (top.empty()) fail(protocol::kInvalidParams, "nothing to reparent");
            const ObjectId parent = ObjectId(params.getNumber("parent", 0));
            const bool keepWorld = params.contains("keepWorld") ? params["keepWorld"].asBool() : true;
            Mat4 toParent;  // world -> new parent's space
            if (parent != kNoObject) {
                const SceneObject* p = scene_.find(parent);
                if (!p) fail(protocol::kNotFound, "parent not found");
                for (ObjectId a = parent; a != kNoObject; a = scene_.find(a)->parent)
                    if (std::find(top.begin(), top.end(), a) != top.end())
                        fail(protocol::kInvalidParams, "cannot parent '" + scene_.find(a)->name + "' under itself or its own descendant '" + p->name + "'");
                auto inv = scene_.worldMatrix(parent).inverse();
                if (!inv) fail(protocol::kInvalidParams, "parent '" + p->name + "' has a degenerate transform");
                toParent = *inv;
            }
            std::vector<SceneObject> afters;
            for (ObjectId id : top) {
                SceneObject o = *scene_.find(id);
                if (o.parent == parent) continue;
                if (keepWorld) {
                    auto t = Transform::fromMatrix(toParent * scene_.worldMatrix(id));
                    if (!t) fail(protocol::kInvalidParams, "'" + o.name + "' cannot keep its world transform under that parent (it would be sheared); nothing was changed");
                    o.transform = *t;
                }
                o.parent = parent;
                afters.push_back(o);
            }
            Json r = Json::object();
            r["ids"] = idsJson(top);
            r["changed"] = double(afters.size());
            if (afters.empty()) return r;  // already there: no undo step
            const std::string target = parent == kNoObject ? std::string("scene root") : scene_.find(parent)->name;
            SceneEdit edit("Parent " + (afters.size() == 1 ? afters[0].name : std::to_string(afters.size()) + " objects") + " to " + target);
            for (const auto& o : afters) edit.recordUpdate(scene_, o);
            commit(std::move(edit), events, "reparent");
            select(top);
            return r;
        }
        if (method == "object.duplicate") {
            // Copies each selected hierarchy with fresh ids, offset in its parent's space; selects the copies.
            const auto top = roots(idList(params, "ids"));
            if (top.empty()) fail(protocol::kInvalidParams, "nothing to duplicate");
            const Vec3 offset = params.contains("offset") ? vec3FromJson(params["offset"]) : Vec3{1, 0, 0};
            SceneEdit edit(top.size() == 1 ? "Duplicate " + scene_.find(top[0])->name : "Duplicate " + std::to_string(top.size()) + " objects");
            std::vector<ObjectId> newRoots;
            for (ObjectId root : top) {
                std::map<ObjectId, ObjectId> map;
                for (ObjectId sid : scene_.withDescendants({root})) {
                    SceneObject o = *scene_.find(sid);
                    o.id = scene_.allocateId();
                    map[sid] = o.id;
                    o.generator.reset();  // a copy is user content, not generator output
                    if (sid == root) {
                        o.name += " copy";
                        o.transform.position = o.transform.position + offset;
                        newRoots.push_back(o.id);
                    } else {
                        o.parent = map.at(o.parent);
                    }
                    edit.recordCreate(o);
                }
            }
            commit(std::move(edit), events, "duplicate");
            select(newRoots);
            Json r = Json::object();
            r["ids"] = idsJson(newRoots);
            return r;
        }
        if (method == "object.group") {
            // New Group at the members' centroid under their common parent; members keep their world positions.
            const auto top = roots(idList(params, "ids"));
            if (top.empty()) fail(protocol::kInvalidParams, "nothing to group");
            const ObjectId parent = scene_.find(top[0])->parent;
            Vec3 c{0, 0, 0};
            for (ObjectId id : top) {
                if (scene_.find(id)->parent != parent) fail(protocol::kInvalidParams, "objects must share the same parent to be grouped");
                c = c + scene_.find(id)->transform.position;
            }
            c = c * (1.0f / float(top.size()));
            SceneObject g;
            g.id = scene_.allocateId();
            g.name = params.getString("name", "Group");
            g.type = ObjectType::Group;
            g.shape = Shape::None;
            g.parent = parent;
            g.transform.position = c;
            SceneEdit edit("Group " + std::to_string(top.size()) + " objects");
            edit.recordCreate(g);
            for (ObjectId id : top) {
                SceneObject o = *scene_.find(id);
                o.parent = g.id;
                o.transform.position = o.transform.position - c;
                edit.recordUpdate(scene_, o);
            }
            commit(std::move(edit), events, "group");
            select({g.id});
            Json r = Json::object();
            r["id"] = g.id;
            return r;
        }
        // object.ungroup: children move to the group's parent with unchanged world positions; the group is deleted.
        const ObjectId gid = ObjectId(params.getNumber("id", 0));
        const SceneObject* g = scene_.find(gid);
        if (!g) fail(protocol::kNotFound, "object not found");
        if (g->type != ObjectType::Group) fail(protocol::kInvalidParams, "only groups can be ungrouped");
        if (g->transform.rotation != Vec3{0, 0, 0} || g->transform.scale != Vec3{1, 1, 1})
            fail(protocol::kInvalidParams, "ungroup needs an unrotated, unscaled group (world transforms would change)");
        const auto kids = scene_.childrenOf(gid);
        SceneEdit edit("Ungroup " + g->name);
        for (ObjectId k : kids) {
            SceneObject o = *scene_.find(k);
            o.parent = g->parent;
            o.transform.position = o.transform.position + g->transform.position;
            edit.recordUpdate(scene_, o);
        }
        edit.recordDelete(scene_, gid);
        commit(std::move(edit), events, "ungroup");
        select(kids);
        Json r = Json::object();
        r["ids"] = idsJson(kids);
        return r;
    }
    if (method == "templates.list") {
        Json arr = Json::array();
        for (const auto& t : listTemplates(templatesDir)) {
            Json j = Json::object();
            j["name"] = t.name;
            j["description"] = t.description;
            j["builtin"] = t.builtin;
            j["objects"] = double(t.scene.objects().size());
            arr.push_back(j);
        }
        Json r = Json::object();
        r["templates"] = arr;
        r["userDir"] = templatesDir.string();
        return r;
    }
    if (method == "project.saveTemplate") {
        // Saves the current scene as a user template (not undoable: it does not change the project).
        requireProject();
        if (templatesDir.empty()) fail(protocol::kInvalidParams, "no user template directory (engine host --templates)");
        SceneTemplate t;
        t.name = params.getString("name");
        t.description = params.getString("description");
        if (t.name.empty() || t.name.size() > 64) fail(protocol::kInvalidParams, "template name must be 1-64 characters");
        for (const auto& b : builtinTemplates())
            if (b.name == t.name) fail(protocol::kInvalidParams, "'" + t.name + "' is a built-in template");
        const fs::path file = templatesDir / templateFileName(t.name);
        if (fs::exists(file) && !params.getBool("overwrite", false)) fail(protocol::kInvalidParams, "template '" + t.name + "' already exists");
        t.scene = scene_;
        t.scene.name = "Main";
        fs::create_directories(templatesDir);
        writeFileAtomic(file, t.toJson().dump(1) + "\n");
        Json r = Json::object();
        r["name"] = t.name;
        r["path"] = file.string();
        r["objects"] = double(t.scene.objects().size());
        return r;
    }
    if (method == "project.autosave") {  // writes a recovery point now (also done periodically by tick())
        requireProject();
        Json r = Json::object();
        r["written"] = writeAutosave();
        return r;
    }
    if (method == "project.recover" || method == "project.discardRecovery") {
        requireProject();
        auto rec = readRecovery();
        if (!rec) fail(protocol::kNotFound, "no recovery point for this project");
        Json r = Json::object();
        if (method == "project.discardRecovery") {
            removeAutosave();
            r["discarded"] = true;
            return r;
        }
        // Applies the recovery point as one undoable edit: the project becomes dirty until it is saved.
        SceneEdit edit("Recover unsaved changes");
        const Scene& to = *rec;
        std::vector<ObjectId> roots;
        for (const auto& [id, o] : to.objects())
            if (o.parent == kNoObject) roots.push_back(id);
        for (ObjectId id : to.withDescendants(roots)) {  // creates and updates, parents first
            const SceneObject& o = *to.find(id);
            if (!scene_.contains(id)) edit.recordCreate(o);
            else if (*scene_.find(id) != o) edit.recordUpdate(scene_, o);
        }
        for (const auto& [id, o] : scene_.objects())
            if (!to.contains(id)) edit.recordDelete(scene_, id);
        for (const Recipe& rcp : to.recipes)
            if (!scene_.findRecipe(rcp.id) || scene_.findRecipe(rcp.id)->toJson() != rcp.toJson())
                edit.recordRecipe(scene_, rcp, rcp.id);
        for (const Recipe& rcp : scene_.recipes)
            if (!to.findRecipe(rcp.id)) edit.recordRecipe(scene_, std::nullopt, rcp.id);
        commit(std::move(edit), events, "recover");
        r["recovered"] = true;
        r["objects"] = double(scene_.objects().size());
        return r;
    }
    if (method == "prefab.create") {
        // Saves an object and its descendants as <project>/Assets/Prefabs/<name>.tprefab and links the source
        // objects to it (one undoable step; the file itself is not removed by undo).
        requireProject();
        const ObjectId root = ObjectId(params.getNumber("id", 0));
        if (!scene_.contains(root)) fail(protocol::kNotFound, "object not found");
        std::string name = params.getString("name", scene_.find(root)->name);
        std::string safe;
        for (char ch : name) safe += (std::isalnum(uint8_t(ch)) || ch == ' ' || ch == '-' || ch == '_') ? ch : '_';
        while (!safe.empty() && safe.back() == ' ') safe.pop_back();
        if (safe.empty() || safe.size() > 64) fail(protocol::kInvalidParams, "prefab name must be 1-64 characters");
        const std::string rel = "Prefabs/" + safe + ".tprefab";
        const fs::path file = project_->root / "Assets" / rel;
        if (fs::exists(file) && !params.getBool("overwrite", false)) fail(protocol::kInvalidParams, "prefab '" + rel + "' already exists");
        Prefab p = Prefab::fromSubtree(scene_, root, name, newPrefabId());
        fs::create_directories(file.parent_path());
        writeFileAtomic(file, p.toJson().dump(1) + "\n");
        SceneEdit edit("Create prefab " + name);
        const auto tree = scene_.withDescendants({root});
        for (size_t i = 0; i < tree.size(); ++i) {
            SceneObject o = *scene_.find(tree[i]);
            o.prefab = PrefabLink{p.id, ObjectId(i + 1)};
            edit.recordUpdate(scene_, o);
        }
        commit(std::move(edit), events, "prefab");
        Json r = Json::object();
        r["prefabId"] = p.id;
        r["path"] = rel;
        r["objects"] = double(tree.size());
        return r;
    }
    if (method == "prefab.instantiate") {
        // Places a copy of a prefab (path relative to <project>/Assets) with fresh ids: one undoable step.
        requireProject();
        std::string rel;
        try {
            rel = assets::resolveReference("", params.getString("path"));
        } catch (const std::exception& e) {
            fail(protocol::kInvalidParams, e.what());
        }
        if (rel.empty() || fs::path(rel).extension() != ".tprefab") fail(protocol::kInvalidParams, "path must name a .tprefab inside Assets/");
        Prefab p;
        try {
            p = readPrefabFile(project_->root / "Assets" / rel);
        } catch (const std::exception& e) {
            fail(protocol::kInvalidParams, std::string("cannot read prefab: ") + e.what());
        }
        const ObjectId parent = ObjectId(params.getNumber("parent", 0));
        if (parent != kNoObject && !scene_.contains(parent)) fail(protocol::kNotFound, "parent not found");
        std::optional<Vec3> pos;
        if (params.contains("position")) pos = vec3FromJson(params["position"]);
        const auto objs = p.instantiate(scene_, parent, pos);
        SceneEdit edit("Instantiate " + p.name);
        for (const auto& o : objs) edit.recordCreate(o);
        commit(std::move(edit), events, "prefab");
        Json ids = Json::array();
        for (const auto& o : objs) ids.push_back(o.id);
        Json r = Json::object();
        r["root"] = objs.front().id;
        r["ids"] = ids;
        r["prefabId"] = p.id;
        return r;
    }
    if (method == "engine.shutdown") return Json::object();  // the transport loop acts on it
    fail(protocol::kMethodNotFound, "unknown method: " + method);
}

}  // namespace titan
