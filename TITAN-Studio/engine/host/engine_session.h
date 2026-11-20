// TITAN Engine Host — the authoritative owner of project and scene state. Transport-free:
// it consumes protocol requests and produces responses plus broadcast events, so it can be
// tested in-process and served over any IPC transport. Original TITAN code.
#pragma once

#include "extension_host.h"

#include "titan/edit.h"
#include "titan/json.h"
#include "titan/project.h"
#include "titan/scene.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace titan {

struct SessionMetrics {
    uint64_t requests = 0;
    uint64_t errors = 0;
    double totalRequestUs = 0;
    double maxRequestUs = 0;
    double lastOpenMs = 0;
    double lastSaveMs = 0;
    std::map<std::string, std::pair<uint64_t, double>> perMethod;  // count, total us
};

class EngineSession {
public:
    using LogFn = std::function<void(int level, const std::string& source, const std::string& message)>;

    // `extensionStateFile` (optional) persists the user's disabled extensions: {"format":1,"disabled":[ids]}.
    explicit EngineSession(std::vector<std::filesystem::path> extensionDirs, LogFn log = {},
                           std::filesystem::path extensionStateFile = {});

    // Handles one request. Events to broadcast to all negotiated clients are appended to
    // `events`. Never throws: failures become protocol error responses.
    Json handle(const Json& request, std::vector<Json>& events);

    bool hasProject() const { return project_.has_value(); }
    const Scene& scene() const { return scene_; }
    const SessionMetrics& metrics() const { return metrics_; }
    ExtensionHost& extensions() { return extensions_; }

    // Crash recovery: called by the host loop. Writes <project>/Saved/Autosave.json (atomically) when the scene
    // changed since the last save and the last recovery point is at least `autosaveInterval` old.
    void tick();
    std::chrono::milliseconds autosaveInterval{20000};
    std::filesystem::path templatesDir;  // user scene templates (--templates); empty: built-ins only

private:
    struct Proposal {
        std::string id;
        std::string extension;
        std::string recipeId;  // recipe being created or regenerated
        Json recipe;
        std::vector<SceneObject> objects;  // ids are provisional (0) until approval
    };

    Json dispatch(const std::string& method, const Json& params, std::vector<Json>& events);
    static const std::vector<std::string>& supportedCapabilities();  // advertised in session.hello
    void requireProject() const;
    void commit(SceneEdit edit, std::vector<Json>& events, const std::string& reason);
    Json stateJson() const;
    Json sceneJson() const;
    void emitChanged(const std::vector<ObjectId>& ids, std::vector<Json>& events, const std::string& reason);
    void writeEditorState();
    void closeProject();
    std::filesystem::path autosaveFile() const;
    bool writeAutosave();
    void removeAutosave();
    std::optional<Scene> readRecovery() const;  // the recovery scene if it exists, is valid and differs

    Json openProject(const std::filesystem::path& path, std::vector<Json>& events);
    Json objectUpdate(const Json& params, std::vector<Json>& events);
    Json objectUpdateMany(const std::string& method, const Json& params, std::vector<Json>& events);
    SceneObject applySet(const SceneObject& cur, const Json& set, std::string& what) const;
    Json objectCreate(const Json& params, std::vector<Json>& events);
    Json objectDelete(const Json& params, std::vector<Json>& events);
    Json worldPreview(const Json& params);
    Json worldApprove(const Json& params, std::vector<Json>& events);

    ExtensionHost extensions_;
    LogFn log_;
    std::optional<ProjectInfo> project_;
    Scene scene_;
    UndoStack undo_;
    std::vector<ObjectId> selection_;
    Json editorState_ = Json::object();
    std::optional<Proposal> proposal_;
    uint64_t proposalCounter_ = 0;
    SessionMetrics metrics_;
    uint64_t autosavedRevision_ = 0;
    std::filesystem::path extensionStateFile_;
    std::chrono::steady_clock::time_point lastAutosave_{};
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};

uint64_t residentMemoryBytes();

}  // namespace titan
