// TITAN Engine — scene templates. Original TITAN code.
#include "titan/scene_template.h"

#include "titan/project.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>

namespace titan {

Json SceneTemplate::toJson() const {
    Json j = Json::object();
    j["format"] = kFormatVersion;
    j["name"] = name;
    j["description"] = description;
    j["scene"] = scene.toJson();
    return j;
}

SceneTemplate SceneTemplate::fromJson(const Json& j) {
    if (!j.isObject() || j.getNumber("format", 0) != kFormatVersion) throw std::invalid_argument("unsupported template format");
    SceneTemplate t;
    t.name = j.getString("name");
    t.description = j.getString("description");
    if (t.name.empty() || t.name.size() > 64) throw std::invalid_argument("template name must be 1-64 characters");
    t.scene = Scene::fromJson(j["scene"]);
    return t;
}

std::vector<SceneTemplate> builtinTemplates() {
    std::vector<SceneTemplate> v;
    SceneTemplate def{"Default", "Ground, sun, camera and a few sample objects.", true, Scene::makeDefault("Main")};
    v.push_back(def);
    SceneTemplate ground{"Ground and Light", "A ground plane, a sun and a camera.", true, Scene::makeDefault("Main")};
    std::vector<ObjectId> drop;
    for (const auto& [id, o] : ground.scene.objects())
        if (o.name != "Ground" && o.name != "Sun" && o.name != "Main Camera") drop.push_back(id);
    for (ObjectId id : drop) ground.scene.erase(id);
    v.push_back(ground);
    SceneTemplate empty{"Empty", "No objects.", true, Scene{}};
    empty.scene.name = "Main";
    v.push_back(empty);
    return v;
}

std::string templateFileName(const std::string& name) {
    std::string s;
    for (char c : name) s += (std::isalnum(uint8_t(c)) || c == ' ' || c == '-' || c == '_') ? c : '_';
    return s + ".ttemplate";
}

std::vector<SceneTemplate> listTemplates(const std::filesystem::path& dir) {
    std::vector<SceneTemplate> v = builtinTemplates();
    std::set<std::string> names;
    for (const auto& t : v) names.insert(t.name);
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return v;
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.is_regular_file() && !e.is_symlink() && e.path().extension() == ".ttemplate") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        try {
            SceneTemplate t = SceneTemplate::fromJson(readJsonFile(f));
            if (names.insert(t.name).second) v.push_back(std::move(t));
        } catch (const std::exception&) {
            // an invalid template file is skipped, never offered
        }
    }
    return v;
}

}  // namespace titan
