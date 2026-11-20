// TITAN Engine — reusable scene templates for new projects. Original TITAN code.
//
// A template is a named, validated scene: {"format":1,"name":...,"description":...,"scene":<Scene JSON>}
// stored as <dir>/<name>.ttemplate. Three templates are built in (Default, Empty, Ground and Light);
// user templates live in a directory chosen by the editor (the engine host's --templates option).
#pragma once

#include "titan/scene.h"

#include <filesystem>
#include <string>
#include <vector>

namespace titan {

struct SceneTemplate {
    static constexpr int kFormatVersion = 1;
    std::string name;
    std::string description;
    bool builtin = false;
    Scene scene;

    Json toJson() const;
    static SceneTemplate fromJson(const Json& j);  // validates the scene like a scene file
};

std::vector<SceneTemplate> builtinTemplates();
// Built-ins followed by valid *.ttemplate files in `dir` (invalid files are skipped). Names are unique;
// a user template never replaces a built-in one.
std::vector<SceneTemplate> listTemplates(const std::filesystem::path& dir);
// File name for a template name: letters, digits, space, '-' and '_' kept, others replaced by '_'.
std::string templateFileName(const std::string& name);

}  // namespace titan
