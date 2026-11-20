// TITAN Engine Host — native extension discovery, validation and lifecycle.
// Original TITAN code.
#pragma once

#include "titan/extension_api.h"
#include "titan/json.h"

#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace titan {

struct SemVer {
    int major = 0, minor = 0, patch = 0;
    static std::optional<SemVer> parse(const std::string& s);  // "1.2.3" (pre-release suffix ignored)
    friend bool operator<(const SemVer& a, const SemVer& b) {
        if (a.major != b.major) return a.major < b.major;
        if (a.minor != b.minor) return a.minor < b.minor;
        return a.patch < b.patch;
    }
};

struct ExtensionManifest {
    std::string id, name, version, publisher, license, category;
    uint32_t apiVersion = 0;
    std::string engineMin, engineMaxExclusive;
    std::vector<std::string> platforms, dependencies, capabilities, commands;
    std::string library;  // file name, relative to the manifest directory
    std::filesystem::path directory;
    Json raw;
};

// Validates a manifest document. Returns the list of problems (empty = valid).
std::vector<std::string> validateManifest(const Json& j, ExtensionManifest& out);
std::string currentPlatform();

class ExtensionHost {
public:
    using LogFn = std::function<void(int level, const std::string& extensionId, const std::string& message)>;

    explicit ExtensionHost(LogFn log);
    ~ExtensionHost();
    ExtensionHost(const ExtensionHost&) = delete;
    ExtensionHost& operator=(const ExtensionHost&) = delete;

    // Discovers <dir>/*/titan-extension.json, validates, resolves dependencies and loads.
    void loadFrom(const std::vector<std::filesystem::path>& dirs);
    void unloadAll();
    // Disabled extensions are discovered and listed but never loaded; extensions depending on them fail.
    void setDisabled(std::set<std::string> ids) { disabled_ = std::move(ids); }
    const std::set<std::string>& disabled() const { return disabled_; }
    bool isKnown(const std::string& id) const { return extensions_.count(id) != 0; }
    // Unloads everything and rescans the directories given to loadFrom (picks up new or removed extensions).
    void reload();

    // Invokes a command; returns the parsed JSON result or throws std::runtime_error.
    Json invoke(const std::string& id, const std::string& command, const Json& params);
    bool isLoaded(const std::string& id) const;
    bool hasCapability(const std::string& id, const std::string& capability) const;
    Json describeAll() const;  // manifests + status for the protocol

private:
    struct Loaded;
    LogFn log_;
    std::map<std::string, std::unique_ptr<Loaded>> extensions_;  // includes failed entries
    std::vector<std::string> loadOrder_;
    std::vector<std::filesystem::path> dirs_;
    std::set<std::string> disabled_;
    TitanHostApi hostApi_{};
    static void hostLog(void* ctx, int level, const char* message);
};

}  // namespace titan
