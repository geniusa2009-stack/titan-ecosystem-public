// TITAN Engine — asset database: scan, hash, import, cache, registry. Original TITAN code.
#include "titan/assets.h"

#include "titan/project.h"
#include "titan/sha256.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <random>
#include <set>
#include <sstream>

namespace titan::assets {
namespace {

bool isHexId(const std::string& s) {
    return s.size() == 32 && std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(uint8_t(c)) && !std::isupper(uint8_t(c)); });
}

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e;
}

int64_t mtimeNs(const fs::path& p) {
    return int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(fs::last_write_time(p).time_since_epoch()).count());
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool cancelled(const std::atomic<bool>* c) { return c && c->load(std::memory_order_relaxed); }

Json recordToJson(const AssetRecord& r) {
    Json j = Json::object();
    j["id"] = r.id;
    j["path"] = r.path;
    j["importer"] = r.importer;
    j["importerVersion"] = r.importerVersion;
    j["sourceHash"] = r.sourceHash;
    j["sourceSize"] = double(r.sourceSize);
    j["sourceMtimeNs"] = std::to_string(r.sourceMtimeNs);  // string: exceeds 2^53
    Json deps = Json::array();
    for (const auto& d : r.dependencies) deps.push_back(d);
    j["dependencies"] = deps;
    j["cacheKey"] = r.cacheKey;
    j["status"] = statusName(r.status);
    if (!r.error.empty()) j["error"] = r.error;
    j["metadata"] = r.metadata.isNull() ? Json::object() : r.metadata;
    return j;
}

AssetRecord recordFromJson(const Json& j) {
    AssetRecord r;
    r.id = j.getString("id");
    r.path = j.getString("path");
    r.importer = j.getString("importer");
    r.importerVersion = int(j.getNumber("importerVersion", 0));
    r.sourceHash = j.getString("sourceHash");
    r.sourceSize = uint64_t(j.getNumber("sourceSize", 0));
    r.sourceMtimeNs = std::stoll(j.getString("sourceMtimeNs", "0"));
    if (j["dependencies"].isArray())
        for (const auto& d : j["dependencies"].asArray()) r.dependencies.push_back(d.asString());
    r.cacheKey = j.getString("cacheKey");
    const std::string st = j.getString("status", "ok");
    r.status = st == "failed" ? Status::Failed : st == "missing" ? Status::Missing : Status::Ok;
    r.error = j.getString("error");
    r.metadata = j["metadata"];
    if (!isHexId(r.id) || r.path.empty()) throw std::runtime_error("bad asset record");
    return r;
}

}  // namespace

const char* statusName(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::Failed: return "failed";
        case Status::Missing: return "missing";
    }
    return "?";
}

AssetDatabase::AssetDatabase(fs::path projectRoot, Options options)
    : root_(std::move(projectRoot)), options_(options), importers_(builtinImporters()) {
    load();
}

void AssetDatabase::load() {
    byId_.clear();
    const fs::path db = libraryDir() / "AssetDB.json";
    if (!fs::exists(db)) return;
    try {
        const Json j = readJsonFile(db);
        if (j.getNumber("format", 0) != kDatabaseFormat) return;  // unknown format: rebuild from .tmeta ids
        for (const auto& r : j["assets"].asArray()) {
            AssetRecord rec = recordFromJson(r);
            byId_[rec.id] = std::move(rec);
        }
    } catch (const std::exception&) {
        byId_.clear();  // a corrupt registry is rebuilt; ids survive in the .tmeta sidecars
    }
}

void AssetDatabase::save() const {
    Json j = Json::object();
    j["format"] = kDatabaseFormat;
    Json arr = Json::array();
    for (const auto& r : records()) arr.push_back(recordToJson(r));
    j["assets"] = arr;
    fs::create_directories(libraryDir());
    writeFileAtomic(libraryDir() / "AssetDB.json", j.dump(1) + "\n");
}

std::string AssetDatabase::newId() {
    static std::mt19937_64 rng{std::random_device{}() ^ uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())};
    static const char* d = "0123456789abcdef";
    for (;;) {
        std::string s;
        for (int i = 0; i < 2; ++i) {
            const uint64_t v = rng();
            for (int k = 0; k < 16; ++k) s += d[(v >> (k * 4)) & 15];
        }
        if (!byId_.count(s)) return s;
    }
}

const Importer* AssetDatabase::importerFor(const std::string& relPath) const {
    const std::string e = lowerExt(fs::path(relPath));
    for (const auto& i : importers_)
        if (i->handles(e)) return i.get();
    return nullptr;
}

std::string AssetDatabase::cacheKeyFor(const AssetRecord& r, const Json& settings) const {
    std::vector<std::string> depHashes;
    // A dependency contributes its availability and current path as well as its content: deleting it, or moving
    // it (even with its .tmeta and unchanged bytes), changes the key, so the referring asset re-resolves its
    // references instead of reusing a product whose reference no longer points anywhere.
    for (const auto& d : r.dependencies) {
        const AssetRecord* dep = find(d);
        depHashes.push_back(dep && dep->status != Status::Missing ? dep->path + "#" + dep->sourceHash : "missing:" + d);
    }
    std::sort(depHashes.begin(), depHashes.end());
    Sha256 h;
    h.update("titan-asset-v1\n" + r.importer + "\n" + std::to_string(r.importerVersion) + "\n" + settings.dump() +
             "\n" + r.sourceHash + "\n");
    for (const auto& d : depHashes) h.update(d + "\n");
    return h.hex();
}

fs::path AssetDatabase::artifactPath(const std::string& key) const {
    return libraryDir() / "Artifacts" / key.substr(0, 2) / (key + ".json");
}

const AssetRecord* AssetDatabase::find(const std::string& id) const {
    auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : &it->second;
}

const AssetRecord* AssetDatabase::findByPath(const std::string& relPath) const {
    for (const auto& [id, r] : byId_)
        if (r.path == relPath && r.status != Status::Missing) return &r;
    return nullptr;
}

std::vector<AssetRecord> AssetDatabase::records() const {
    std::vector<AssetRecord> v;
    for (const auto& [id, r] : byId_) v.push_back(r);
    std::sort(v.begin(), v.end(), [](const AssetRecord& a, const AssetRecord& b) { return a.path < b.path; });
    return v;
}

std::vector<std::string> AssetDatabase::dependents(const std::string& id) const {
    std::vector<std::string> out;
    for (const auto& [rid, r] : byId_)
        if (std::find(r.dependencies.begin(), r.dependencies.end(), id) != r.dependencies.end()) out.push_back(rid);
    return out;
}

RefreshStats AssetDatabase::refresh(const std::atomic<bool>* cancel) {
    RefreshStats st;
    const fs::path assets = assetsDir();
    fs::create_directories(assets);

    // 1. Scan: regular files only (symlinks are skipped, never followed), hidden files and sidecars ignored.
    std::vector<std::string> paths;
    for (auto it = fs::recursive_directory_iterator(assets); it != fs::recursive_directory_iterator(); ++it) {
        const fs::directory_entry& e = *it;
        const std::string name = e.path().filename().string();
        if (e.is_symlink() || name.empty() || name[0] == '.') {
            if (e.is_directory() && !e.is_symlink()) it.disable_recursion_pending();
            continue;
        }
        if (!e.is_regular_file() || lowerExt(e.path()) == ".tmeta") continue;
        const std::string rel = fs::relative(e.path(), assets).generic_string();
        if (importerFor(rel)) paths.push_back(rel);
    }
    std::sort(paths.begin(), paths.end());

    // 2. Identity + hash. Ids come from the .tmeta sidecar; a lost sidecar is re-associated by content hash
    //    with a record whose file disappeared (a rename).
    std::map<std::string, Json> settingsById;
    std::set<std::string> seen;
    for (const std::string& rel : paths) {
        if (cancelled(cancel)) { st.cancelled = true; break; }
        ++st.scanned;
        const fs::path file = assets / fs::path(rel);
        const fs::path metaFile = fs::path(file.string() + ".tmeta");
        const uint64_t size = fs::file_size(file);
        const int64_t mt = mtimeNs(file);
        std::string id;
        Json settings = Json::object();
        if (fs::exists(metaFile)) {
            try {
                const Json m = readJsonFile(metaFile);
                id = m.getString("id");
                if (m["settings"].isObject()) settings = m["settings"];
            } catch (const std::exception&) {
                id.clear();
            }
            if (!isHexId(id) || seen.count(id)) id.clear();  // invalid or duplicated (copied file + sidecar)
        }
        AssetRecord* prev = id.empty() ? nullptr : (byId_.count(id) ? &byId_[id] : nullptr);
        std::string hash;
        if (prev && prev->path == rel && prev->sourceSize == size && prev->sourceMtimeNs == mt && !prev->sourceHash.empty()) {
            hash = prev->sourceHash;
        } else if (size > options_.maxSourceBytes) {
            hash = "";  // never read oversized sources
        } else {
            hash = sha256File(file);
            ++st.hashed;
        }
        bool reassociated = false;
        if (id.empty()) {
            for (auto& [rid, r] : byId_)
                if (!seen.count(rid) && !hash.empty() && r.sourceHash == hash && !fs::exists(assets / fs::path(r.path))) {
                    id = rid;
                    reassociated = true;
                    ++st.renamed;
                    break;
                }
            if (id.empty()) id = newId();
            Json m = Json::object();
            m["format"] = kMetaFormat;
            m["id"] = id;
            m["settings"] = settings;
            writeFileAtomic(metaFile, m.dump(1) + "\n");
        }
        seen.insert(id);
        settingsById[id] = settings;
        AssetRecord& r = byId_[id];
        const Importer* imp = importerFor(rel);
        r.id = id;
        if (!reassociated && r.path != rel && !r.path.empty() && r.status != Status::Missing)
            ++st.renamed;  // moved together with its sidecar
        r.path = rel;
        r.importer = imp->id();
        r.importerVersion = imp->version();
        r.sourceSize = size;
        r.sourceMtimeNs = mt;
        r.sourceHash = hash;
        if (r.status == Status::Missing) r.status = Status::Ok;  // back again; step 4 re-validates the product
        if (size > options_.maxSourceBytes) {
            r.status = Status::Failed;
            r.error = "source is " + std::to_string(size) + " bytes, above the limit of " +
                      std::to_string(options_.maxSourceBytes);
        }
    }

    // 3. Records whose source is gone are kept (id, product) but marked missing.
    if (!st.cancelled)
        for (auto& [id, r] : byId_)
            if (!seen.count(id)) {
                if (r.status != Status::Missing) ++st.missing;
                r.status = Status::Missing;
            }

    // 4. Import what changed; reuse intact products whose cache key matches.
    for (const std::string& id : std::vector<std::string>(seen.begin(), seen.end())) {
        if (st.cancelled || cancelled(cancel)) { st.cancelled = true; break; }
        AssetRecord& r = byId_[id];
        if (r.sourceHash.empty()) { ++st.failed; continue; }  // oversized: error already recorded
        const Json& settings = settingsById[id];
        const std::string key = cacheKeyFor(r, settings);
        if (key == r.cacheKey && r.status != Status::Failed) {
            bool intact = false;
            try {
                intact = readJsonFile(artifactPath(key)).getString("key") == key;
            } catch (const std::exception&) {
            }
            if (intact) { ++st.reused; continue; }
        }
        ImportContext ctx;
        ctx.path = r.path;
        ctx.settings = settings;
        ImportResult res;
        try {
            ctx.bytes = readAll(assets / fs::path(r.path));
            res = importerFor(r.path)->import(ctx);
        } catch (const std::exception& e) {
            res.ok = false;
            res.error = e.what();
        }
        std::vector<std::string> deps;
        if (res.ok)
            for (const auto& p : res.dependencyPaths) {
                const AssetRecord* d = findByPath(p);
                if (!d || !seen.count(d->id)) { res.ok = false; res.error = "missing dependency '" + p + "'"; break; }
                deps.push_back(d->id);
            }
        if (!res.ok) {
            r.status = Status::Failed;  // previous cacheKey/metadata (last valid product) are kept
            r.error = res.error;
            ++st.failed;
            continue;
        }
        std::sort(deps.begin(), deps.end());
        deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
        r.dependencies = deps;
        const std::string newKey = cacheKeyFor(r, settings);
        Json art = Json::object();
        art["format"] = 1;
        art["key"] = newKey;
        art["assetId"] = id;
        art["importer"] = r.importer;
        art["importerVersion"] = r.importerVersion;
        art["sourceHash"] = r.sourceHash;
        art["metadata"] = res.metadata;
        fs::create_directories(artifactPath(newKey).parent_path());
        writeFileAtomic(artifactPath(newKey), art.dump(1) + "\n");
        r.cacheKey = newKey;
        r.metadata = res.metadata;
        r.status = Status::Ok;
        r.error.clear();
        ++st.imported;
    }

    // Every source hash is final before step 4, so a changed dependency already changes the cache key of the
    // assets that read it: no ordering between imports is needed. Failed assets retry on every refresh.
    save();
    return st;
}

int AssetDatabase::collectGarbage() {
    std::set<std::string> live;
    for (const auto& [id, r] : byId_)
        if (!r.cacheKey.empty()) live.insert(r.cacheKey + ".json");
    int removed = 0;
    const fs::path dir = libraryDir() / "Artifacts";
    if (!fs::exists(dir)) return 0;
    std::vector<fs::path> dead;
    for (const auto& e : fs::recursive_directory_iterator(dir))
        if (e.is_regular_file() && !e.is_symlink() && !live.count(e.path().filename().string())) dead.push_back(e.path());
    for (const auto& p : dead)
        if (fs::remove(p)) ++removed;
    return removed;
}

}  // namespace titan::assets
