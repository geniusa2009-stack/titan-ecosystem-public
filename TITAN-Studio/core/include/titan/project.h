// TITAN Engine — project layout and persistence. Original TITAN code.
//
// <project>/project.titan          project manifest (JSON)
// <project>/Scenes/<Name>.tscene   scene files (JSON, see Scene::toJson)
// <project>/Saved/editor.json      per-user editor state (camera, selection); not content
#pragma once

#include "titan/json.h"
#include "titan/scene.h"

#include <filesystem>
#include <string>

namespace titan {

struct ProjectInfo {
    static constexpr int kFormatVersion = 1;
    std::filesystem::path root;
    std::string name;
    std::string scenePath = "Scenes/Main.tscene";  // relative to root
    std::string engineVersion;

    std::filesystem::path manifestFile() const { return root / "project.titan"; }
    std::filesystem::path sceneFile() const { return root / scenePath; }
    std::filesystem::path editorStateFile() const { return root / "Saved" / "editor.json"; }
    Json toJson() const;
};

class ProjectError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Creates a new project with a default scene. Fails if the directory exists and is not empty.
// `initial` (e.g. a scene template) replaces the default scene when given.
ProjectInfo createProject(const std::filesystem::path& root, const std::string& name, const Scene* initial = nullptr);
// Reads and validates the manifest. Throws ProjectError.
ProjectInfo readProject(const std::filesystem::path& root);
Scene loadScene(const ProjectInfo& info);
void saveScene(const ProjectInfo& info, const Scene& scene);

Json readJsonFile(const std::filesystem::path& file);
// Atomic replace: write to a sibling temp file, flush, then rename over the target.
void writeFileAtomic(const std::filesystem::path& file, const std::string& contents);

}  // namespace titan
