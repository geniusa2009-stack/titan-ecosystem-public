// TITAN Engine — project persistence. Original TITAN code.
#include "titan/project.h"
#include "titan/version.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace titan {

Json ProjectInfo::toJson() const {
    Json j = Json::object();
    j["format"] = "titan.project";
    j["formatVersion"] = kFormatVersion;
    j["name"] = name;
    j["engineVersion"] = engineVersion.empty() ? std::string(kEngineVersion) : engineVersion;
    j["defaultScene"] = scenePath;
    return j;
}

Json readJsonFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw ProjectError("cannot open " + file.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    try {
        return Json::parse(ss.str());
    } catch (const JsonError& e) {
        throw ProjectError(file.filename().string() + ": " + e.what());
    }
}

void writeFileAtomic(const fs::path& file, const std::string& contents) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) throw ProjectError("cannot create " + file.parent_path().string() + ": " + ec.message());
    fs::path tmp = file;
    tmp += ".tmp";
    {
        FILE* f = std::fopen(tmp.string().c_str(), "wb");
        if (!f) throw ProjectError("cannot write " + tmp.string());
        bool ok = std::fwrite(contents.data(), 1, contents.size(), f) == contents.size();
        ok = std::fflush(f) == 0 && ok;
#if !defined(_WIN32)
        ok = ::fsync(fileno(f)) == 0 && ok;
#endif
        ok = std::fclose(f) == 0 && ok;
        if (!ok) {
            fs::remove(tmp, ec);
            throw ProjectError("short write to " + tmp.string());
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw ProjectError("cannot replace " + file.string() + ": " + ec.message());
    }
}

ProjectInfo createProject(const fs::path& root, const std::string& name, const Scene* initial) {
    if (name.empty()) throw ProjectError("project name must not be empty");
    std::error_code ec;
    if (fs::exists(root, ec) && !fs::is_empty(root, ec))
        throw ProjectError("directory is not empty: " + root.string());
    fs::create_directories(root / "Scenes", ec);
    if (ec) throw ProjectError("cannot create project directory: " + ec.message());
    ProjectInfo info;
    info.root = fs::absolute(root).lexically_normal();
    info.name = name;
    info.engineVersion = kEngineVersion;
    writeFileAtomic(info.manifestFile(), info.toJson().dump(2) + "\n");
    saveScene(info, initial ? *initial : Scene::makeDefault("Main"));
    return info;
}

ProjectInfo readProject(const fs::path& root) {
    ProjectInfo info;
    info.root = fs::absolute(root).lexically_normal();
    Json j = readJsonFile(info.manifestFile());
    if (j.getString("format") != "titan.project") throw ProjectError("not a TITAN project manifest");
    int v = static_cast<int>(j["formatVersion"].asInt());
    if (v < 1 || v > ProjectInfo::kFormatVersion)
        throw ProjectError("unsupported project formatVersion " + std::to_string(v));
    info.name = j.getString("name", info.root.filename().string());
    info.engineVersion = j.getString("engineVersion");
    info.scenePath = j.getString("defaultScene", info.scenePath);
    // The scene path comes from a file on disk: refuse anything that escapes the project.
    fs::path rel = fs::path(info.scenePath).lexically_normal();
    if (rel.is_absolute() || rel.empty() || *rel.begin() == "..")
        throw ProjectError("defaultScene must be a relative path inside the project");
    return info;
}

Scene loadScene(const ProjectInfo& info) {
    Json j = readJsonFile(info.sceneFile());
    try {
        return Scene::fromJson(j);
    } catch (const std::exception& e) {
        throw ProjectError(info.scenePath + ": " + e.what());
    }
}

void saveScene(const ProjectInfo& info, const Scene& scene) {
    writeFileAtomic(info.sceneFile(), scene.toJson().dump(2) + "\n");
}

}  // namespace titan
