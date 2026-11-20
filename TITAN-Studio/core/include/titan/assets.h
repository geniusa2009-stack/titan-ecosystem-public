// TITAN Engine — asset pipeline (Issue #5, milestone M1). Original TITAN code, no Qt/UE dependency.
//
// Project layout:
//   <project>/Assets/...                source files (any sub-folders)
//   <project>/Assets/<file>.tmeta       sidecar per source: stable asset id + importer settings (JSON). Moving
//                                       or renaming a source together with its .tmeta keeps the id.
//   <project>/Library/AssetDB.json      registry of imported assets (written atomically)
//   <project>/Library/Artifacts/xx/<key>.json   content-addressed import products
//
// Incremental rules:
//   * A source is hashed only when its size or mtime changed (SHA-256 of the content otherwise reused).
//   * cache key = SHA-256(importer id, importer version, settings, source hash, hashes of the sources it
//     depends on, sorted). A product whose key exists and whose file is intact is reused, never re-imported.
//   * Changing a source re-imports it and the assets that depend on it, nothing else.
//   * A failed or cancelled import keeps the previous valid product; nothing is half-written.
//   * Sources whose .tmeta was lost are re-associated with their old id by content hash (rename detection).
// Security: only regular files under Assets/ are read (symlinks are ignored), files above maxSourceBytes
// are refused, every parser is bounds-checked, and references ("uri") may not escape Assets/.
#pragma once

#include "titan/json.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace titan::assets {

namespace fs = std::filesystem;

constexpr int kDatabaseFormat = 1;
constexpr int kMetaFormat = 1;

enum class Status { Ok, Failed, Missing };
const char* statusName(Status s);

struct AssetRecord {
    std::string id;              // 32 lowercase hex characters, stable for the asset's lifetime
    std::string path;            // relative to Assets/, '/' separators
    std::string importer;        // importer id, e.g. "image.png"
    int importerVersion = 0;
    std::string sourceHash;      // SHA-256 of the source content
    uint64_t sourceSize = 0;
    int64_t sourceMtimeNs = 0;
    std::vector<std::string> dependencies;  // asset ids this asset reads (e.g. textures of a glTF)
    std::string cacheKey;        // key of the last successful product ("" if never imported)
    Status status = Status::Ok;
    std::string error;           // last import error (kept while the previous product stays valid)
    Json metadata;               // importer output summary (dimensions, counts, ...)
};

struct RefreshStats {
    int scanned = 0, hashed = 0, imported = 0, reused = 0, failed = 0, missing = 0, renamed = 0;
    bool cancelled = false;
};

struct ImportContext {
    std::string path;            // relative to Assets/
    std::string bytes;           // whole source content (already size-limited)
    Json settings;               // from the .tmeta
};

struct ImportResult {
    bool ok = false;
    std::string error;
    Json metadata = Json::object();
    std::vector<std::string> dependencyPaths;  // relative to Assets/, already validated by the importer
};

class Importer {
public:
    virtual ~Importer() = default;
    virtual std::string id() const = 0;
    virtual int version() const = 0;
    virtual bool handles(const std::string& lowerExtension) const = 0;
    virtual ImportResult import(const ImportContext& ctx) const = 0;
};

// Built-in importers: glTF 2.0 (.gltf/.glb), PNG, JPEG, WAV (PCM / IEEE float).
std::vector<std::unique_ptr<Importer>> builtinImporters();

// Resolves a reference `uri` written inside `fromPath` (both relative to Assets/) to an Assets/-relative
// path. Returns "" for data: URIs and throws std::runtime_error for absolute paths, schemes or escapes.
std::string resolveReference(const std::string& fromPath, const std::string& uri);

struct Options {
    uint64_t maxSourceBytes = 512ull << 20;  // refuse larger sources (512 MiB)
};

class AssetDatabase {
public:
    explicit AssetDatabase(fs::path projectRoot, Options options = {});

    // Scans Assets/, imports what changed, reuses everything else, saves the registry atomically.
    // `cancel` is polled between assets and while hashing; a cancelled refresh saves the finished work only.
    RefreshStats refresh(const std::atomic<bool>* cancel = nullptr);

    const AssetRecord* find(const std::string& id) const;
    const AssetRecord* findByPath(const std::string& relPath) const;
    std::vector<AssetRecord> records() const;  // sorted by path
    std::vector<std::string> dependents(const std::string& id) const;
    fs::path artifactPath(const std::string& cacheKey) const;

    // Removes artifacts no record references. Only touches <project>/Library/Artifacts. Returns files removed.
    int collectGarbage();

    fs::path assetsDir() const { return root_ / "Assets"; }
    fs::path libraryDir() const { return root_ / "Library"; }

private:
    void load();
    void save() const;
    std::string newId();
    std::string cacheKeyFor(const AssetRecord& r, const Json& settings) const;
    const Importer* importerFor(const std::string& relPath) const;

    fs::path root_;
    Options options_;
    std::vector<std::unique_ptr<Importer>> importers_;
    std::map<std::string, AssetRecord> byId_;
};

}  // namespace titan::assets
