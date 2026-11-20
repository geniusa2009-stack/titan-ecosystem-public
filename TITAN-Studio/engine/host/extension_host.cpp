// TITAN Engine Host — extension host implementation. Original TITAN code.
#include "extension_host.h"

#include "titan/project.h"
#include "titan/version.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>

#if defined(_WIN32)
#error "Windows extension loading is not implemented yet"
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace titan {

namespace {
const std::set<std::string> kKnownCapabilities = {"scene.read", "scene.propose", "editor.panel", "log"};
const std::set<std::string> kKnownCategories = {"engine",   "editor",     "ai-tool", "asset-importer",
                                                "world-generation", "rendering", "simulation", "partner-adapter"};

std::vector<std::string> stringList(const Json& j, const char* key, std::vector<std::string>& problems) {
    std::vector<std::string> out;
    const Json* v = j.find(key);
    if (!v) return out;
    if (!v->isArray()) {
        problems.push_back(std::string(key) + " must be an array of strings");
        return out;
    }
    for (const auto& e : v->asArray()) {
        if (e.isString()) out.push_back(e.asString());
        else problems.push_back(std::string(key) + " must contain only strings");
    }
    return out;
}

bool validId(const std::string& id) {
    if (id.empty() || id.size() > 128) return false;
    for (char c : id)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_')) return false;
    return id.front() != '.' && id.back() != '.';
}
}  // namespace

std::optional<SemVer> SemVer::parse(const std::string& s) {
    SemVer v;
    int* parts[3] = {&v.major, &v.minor, &v.patch};
    size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        if (pos >= s.size() || !std::isdigit(static_cast<unsigned char>(s[pos]))) return std::nullopt;
        int n = 0;
        while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
            n = n * 10 + (s[pos++] - '0');
            if (n > 1000000) return std::nullopt;
        }
        *parts[i] = n;
        if (i < 2) {
            if (pos >= s.size() || s[pos] != '.') return std::nullopt;
            ++pos;
        }
    }
    if (pos < s.size() && s[pos] != '-' && s[pos] != '+') return std::nullopt;
    return v;
}

std::string currentPlatform() {
#if defined(__APPLE__)
    const char* os = "macos";
#elif defined(__linux__)
    const char* os = "linux";
#else
    const char* os = "unknown";
#endif
#if defined(__aarch64__) || defined(__arm64__)
    const char* arch = "arm64";
#elif defined(__x86_64__)
    const char* arch = "x64";
#else
    const char* arch = "unknown";
#endif
    return std::string(os) + "-" + arch;
}

std::vector<std::string> validateManifest(const Json& j, ExtensionManifest& m) {
    std::vector<std::string> problems;
    if (!j.isObject()) return {"manifest must be a JSON object"};
    auto str = [&](const char* key, bool required) {
        const Json* v = j.find(key);
        if (!v) {
            if (required) problems.push_back(std::string("missing ") + key);
            return std::string();
        }
        if (!v->isString()) {
            problems.push_back(std::string(key) + " must be a string");
            return std::string();
        }
        return v->asString();
    };
    if (j.getNumber("manifestVersion", 0) != 1) problems.push_back("manifestVersion must be 1");
    m.id = str("id", true);
    m.name = str("name", true);
    m.version = str("version", true);
    m.publisher = str("publisher", true);
    m.license = str("license", true);
    m.category = str("category", true);
    if (!m.id.empty() && !validId(m.id)) problems.push_back("id has invalid characters");
    if (!m.version.empty() && !SemVer::parse(m.version)) problems.push_back("version must be semver");
    if (!m.category.empty() && !kKnownCategories.count(m.category)) problems.push_back("unknown category " + m.category);
    const Json* api = j.find("apiVersion");
    if (!api || !api->isNumber()) problems.push_back("missing apiVersion");
    else m.apiVersion = static_cast<uint32_t>(api->asNumber());
    if (m.apiVersion != TITAN_EXTENSION_API_VERSION)
        problems.push_back("apiVersion " + std::to_string(m.apiVersion) + " not supported (host supports " +
                           std::to_string(TITAN_EXTENSION_API_VERSION) + ")");
    const Json& engine = j["engine"];
    if (!engine.isObject()) {
        problems.push_back("missing engine compatibility range");
    } else {
        m.engineMin = engine.getString("min");
        m.engineMaxExclusive = engine.getString("maxExclusive");
        auto lo = SemVer::parse(m.engineMin), hi = SemVer::parse(m.engineMaxExclusive);
        auto cur = SemVer::parse(kEngineVersion);
        if (!lo || !hi) problems.push_back("engine.min / engine.maxExclusive must be semver");
        else if (cur && (*cur < *lo || !(*cur < *hi)))
            problems.push_back("engine " + std::string(kEngineVersion) + " outside supported range [" + m.engineMin +
                               ", " + m.engineMaxExclusive + ")");
    }
    m.platforms = stringList(j, "platforms", problems);
    m.dependencies = stringList(j, "dependencies", problems);
    m.capabilities = stringList(j, "capabilities", problems);
    m.commands = stringList(j, "commands", problems);
    if (std::find(m.platforms.begin(), m.platforms.end(), currentPlatform()) == m.platforms.end())
        problems.push_back("platform " + currentPlatform() + " not supported by extension");
    for (const auto& c : m.capabilities)
        if (!kKnownCapabilities.count(c)) problems.push_back("unknown capability " + c);
    m.library = j["entry"].getString("library");
    if (m.library.empty()) {
        problems.push_back("missing entry.library");
    } else {
        fs::path lib(m.library);
        // The library must live directly in the extension directory.
        if (lib.is_absolute() || lib.has_parent_path() || m.library.find("..") != std::string::npos)
            problems.push_back("entry.library must be a plain file name inside the extension directory");
    }
    m.raw = j;
    return problems;
}

struct ExtensionHost::Loaded {
    ExtensionManifest manifest;
    std::string status = "discovered";  // loaded | failed | discovered
    std::vector<std::string> problems;
    void* handle = nullptr;
    const TitanExtensionApi* api = nullptr;
};

ExtensionHost::ExtensionHost(LogFn log) : log_(std::move(log)) {
    hostApi_.api_version = TITAN_EXTENSION_API_VERSION;
    hostApi_.engine_version = kEngineVersion;
    hostApi_.host_context = this;
    hostApi_.log = &ExtensionHost::hostLog;
}

ExtensionHost::~ExtensionHost() { unloadAll(); }

void ExtensionHost::hostLog(void* ctx, int level, const char* message) {
    auto* self = static_cast<ExtensionHost*>(ctx);
    if (self && self->log_) self->log_(level, "extension", message ? message : "");
}

void ExtensionHost::loadFrom(const std::vector<fs::path>& dirs) {
    dirs_ = dirs;
    std::error_code ec;
    for (const auto& dir : dirs) {
        if (!fs::is_directory(dir, ec)) continue;
        std::vector<fs::path> entries;
        for (const auto& e : fs::directory_iterator(dir, ec)) entries.push_back(e.path());
        std::sort(entries.begin(), entries.end());  // deterministic discovery order
        for (const auto& path : entries) {
            fs::path manifestFile = path / "titan-extension.json";
            if (!fs::is_regular_file(manifestFile, ec)) continue;
            auto entry = std::make_unique<Loaded>();
            entry->manifest.directory = path;
            try {
                entry->problems = validateManifest(readJsonFile(manifestFile), entry->manifest);
            } catch (const std::exception& e) {
                entry->problems = {e.what()};
            }
            std::string key = entry->manifest.id.empty() ? path.filename().string() : entry->manifest.id;
            if (extensions_.count(key)) entry->problems.push_back("duplicate extension id");
            if (!entry->problems.empty()) entry->status = "failed";
            else if (disabled_.count(key)) entry->status = "disabled";
            if (!extensions_.count(key)) extensions_[key] = std::move(entry);
        }
    }
    // Resolve dependencies by repeated passes (simple topological order).
    bool progress = true;
    while (progress) {
        progress = false;
        for (auto& [id, ext] : extensions_) {
            if (ext->status != "discovered") continue;
            bool ready = true, broken = false;
            std::string depWhy;
            for (const auto& dep : ext->manifest.dependencies) {
                auto it = extensions_.find(dep);
                if (it == extensions_.end()) { broken = true; depWhy = "dependency '" + dep + "' is not installed"; }
                else if (it->second->status == "failed") { broken = true; depWhy = "dependency '" + dep + "' failed to load"; }
                else if (it->second->status == "disabled") { broken = true; depWhy = "dependency '" + dep + "' is disabled"; }
                else if (it->second->status != "loaded") ready = false;
            }
            if (broken) {
                ext->status = "failed";
                ext->problems.push_back(depWhy);
                progress = true;
                continue;
            }
            if (!ready) continue;
            progress = true;
            fs::path lib = ext->manifest.directory / ext->manifest.library;
            ext->handle = ::dlopen(lib.string().c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!ext->handle) {
                const char* err = ::dlerror();
                ext->status = "failed";
                ext->problems.push_back(std::string("dlopen failed: ") + (err ? err : "?"));
                continue;
            }
            auto entryFn = reinterpret_cast<TitanExtensionEntryFn>(::dlsym(ext->handle, TITAN_EXTENSION_ENTRY_SYMBOL));
            const TitanExtensionApi* api = entryFn ? entryFn() : nullptr;
            std::string why;
            if (!api) why = "entry symbol missing";
            else if (api->api_version != TITAN_EXTENSION_API_VERSION) why = "library apiVersion mismatch";
            else if (!api->id || id != api->id) why = "library id does not match manifest id";
            else if (!api->initialize || !api->invoke || !api->shutdown) why = "incomplete API table";
            else if (api->initialize(&hostApi_) != 0) why = "initialize() failed";
            if (!why.empty()) {
                ::dlclose(ext->handle);
                ext->handle = nullptr;
                ext->status = "failed";
                ext->problems.push_back(why);
                continue;
            }
            ext->api = api;
            ext->status = "loaded";
            loadOrder_.push_back(id);
            if (log_) log_(TITAN_LOG_INFO, id, "loaded " + ext->manifest.version);
        }
    }
    for (auto& [id, ext] : extensions_) {
        if (ext->status == "discovered") {
            ext->status = "failed";
            ext->problems.push_back("dependency cycle");
        }
        if (ext->status == "failed" && log_) {
            std::string msg = "not loaded:";
            for (const auto& p : ext->problems) msg += " " + p + ";";
            log_(TITAN_LOG_WARNING, id, msg);
        }
    }
}

void ExtensionHost::reload() {
    unloadAll();
    extensions_.clear();
    const auto dirs = dirs_;
    loadFrom(dirs);
}

void ExtensionHost::unloadAll() {
    for (auto it = loadOrder_.rbegin(); it != loadOrder_.rend(); ++it) {
        auto& ext = extensions_[*it];
        if (ext->api) ext->api->shutdown();
        if (ext->handle) ::dlclose(ext->handle);
        ext->api = nullptr;
        ext->handle = nullptr;
        ext->status = "unloaded";
    }
    loadOrder_.clear();
}

bool ExtensionHost::isLoaded(const std::string& id) const {
    auto it = extensions_.find(id);
    return it != extensions_.end() && it->second->status == "loaded";
}

bool ExtensionHost::hasCapability(const std::string& id, const std::string& capability) const {
    auto it = extensions_.find(id);
    if (it == extensions_.end()) return false;
    const auto& caps = it->second->manifest.capabilities;
    return std::find(caps.begin(), caps.end(), capability) != caps.end();
}

Json ExtensionHost::invoke(const std::string& id, const std::string& command, const Json& params) {
    auto it = extensions_.find(id);
    if (it == extensions_.end() || it->second->status != "loaded")
        throw std::runtime_error("extension not loaded: " + id);
    const auto& cmds = it->second->manifest.commands;
    if (std::find(cmds.begin(), cmds.end(), command) == cmds.end())
        throw std::runtime_error("command not declared in manifest: " + command);
    const char* out = it->second->api->invoke(command.c_str(), params.dump().c_str());
    if (!out) throw std::runtime_error("extension returned null");
    Json result = Json::parse(out);
    if (result.contains("error")) throw std::runtime_error(id + ": " + result["error"].getString("message", "error"));
    return result;
}

Json ExtensionHost::describeAll() const {
    Json arr = Json::array();
    for (const auto& [id, ext] : extensions_) {
        Json j = Json::object();
        j["id"] = id;
        j["name"] = ext->manifest.name;
        j["version"] = ext->manifest.version;
        j["publisher"] = ext->manifest.publisher;
        j["license"] = ext->manifest.license;
        j["category"] = ext->manifest.category;
        j["status"] = ext->status;
        j["enabled"] = !disabled_.count(id);
        Json caps = Json::array();
        for (const auto& c : ext->manifest.capabilities) caps.push_back(c);
        j["capabilities"] = caps;
        Json probs = Json::array();
        for (const auto& p : ext->problems) probs.push_back(p);
        j["problems"] = probs;
        arr.push_back(j);
    }
    return arr;
}

}  // namespace titan
