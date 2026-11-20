// TITAN asset pipeline (Issue #5 M1): importers, incremental database, identity, failure safety.
// Original TITAN code.
#include "titan/assets.h"
#include "titan/project.h"
#include "titan/sha256.h"
#include "titan_test.h"

#include <atomic>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace titan;
using namespace titan::assets;

namespace {
uint32_t crc(const std::string& s) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : s) {
        c ^= ch;
        for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xFFFFFFFFu;
}
std::string be32(uint32_t v) { return {char(v >> 24), char(v >> 16), char(v >> 8), char(v)}; }
std::string le32(uint32_t v) { return {char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; }
std::string le16(uint16_t v) { return {char(v), char(v >> 8)}; }
std::string chunk(const std::string& type, const std::string& data) {
    return be32(uint32_t(data.size())) + type + data + be32(crc(type + data));
}
std::string png(uint32_t w, uint32_t h, char fill = 0) {
    std::string ihdr = be32(w) + be32(h) + std::string{8, 6, 0, 0, 0};
    return std::string("\x89PNG\r\n\x1a\n", 8) + chunk("IHDR", ihdr) + chunk("IDAT", std::string(12, fill)) + chunk("IEND", "");
}
std::string jpeg(uint16_t w, uint16_t h) {
    std::string s = "\xFF\xD8";
    s += std::string("\xFF\xE0", 2) + std::string{0, 16} + std::string("JFIF\0\1\1\0\0\1\0\1\0\0", 14);
    s += std::string("\xFF\xC0", 2) + std::string{0, 17, 8} + std::string{char(h >> 8), char(h)} +
         std::string{char(w >> 8), char(w)} + std::string{3, 1, 0x11, 0, 2, 0x11, 1, 3, 0x11, 1};
    s += std::string("\xFF\xDA", 2) + std::string{0, 12, 3, 1, 0, 2, 0x11, 3, 0x11, 0, 63, 0} + "data" + "\xFF\xD9";
    return s;
}
std::string wav(uint32_t rate, uint16_t channels, uint32_t frames) {
    const uint16_t bits = 16, align = uint16_t(channels * 2);
    const std::string fmt = le16(1) + le16(channels) + le32(rate) + le32(rate * align) + le16(align) + le16(bits);
    const std::string data(size_t(frames) * align, '\0');
    const std::string body = "WAVE" + std::string("fmt ") + le32(16) + fmt + "data" + le32(uint32_t(data.size())) + data;
    return "RIFF" + le32(uint32_t(body.size())) + body;
}
std::string glb(const std::string& json, const std::string& bin) {
    std::string j = json;
    while (j.size() % 4) j += ' ';
    std::string b = bin;
    while (b.size() % 4) b += '\0';
    std::string body = le32(uint32_t(j.size())) + "JSON" + j;
    if (!b.empty()) body += le32(uint32_t(b.size())) + std::string("BIN\0", 4) + b;
    return "glTF" + le32(2) + le32(uint32_t(12 + body.size())) + body;
}
const char* kGltfWithTexture = R"({"asset":{"version":"2.0","generator":"titan-test"},
  "buffers":[{"uri":"cube.bin","byteLength":24}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":24}],
  "images":[{"uri":"../Textures/brick%20wall.png"}],"textures":[{"source":0}],"materials":[{}],
  "meshes":[{"primitives":[]}],"nodes":[{"mesh":0}]})";

struct Project {
    fs::path root;
    Project() {
        root = fs::temp_directory_path() / ("titan-assets-" + std::to_string(getpid()) + "-" + std::to_string(counter()++));
        fs::remove_all(root);
        fs::create_directories(root / "Assets");
    }
    ~Project() { fs::remove_all(root); }
    static int& counter() { static int c = 0; return c; }
    void write(const std::string& rel, const std::string& bytes) {
        const fs::path p = root / "Assets" / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << bytes;
    }
    void touchLater(const std::string& rel) {  // make sure a rewrite is seen as a new mtime
        const fs::path p = root / "Assets" / rel;
        fs::last_write_time(p, fs::last_write_time(p) + std::chrono::seconds(2));
    }
    void standardSet() {
        write("Textures/brick wall.png", png(64, 32));
        write("Textures/rock.jpg", jpeg(320, 200));
        write("Audio/hit.wav", wav(48000, 2, 4800));
        write("Models/cube.bin", std::string(24, 'x'));
        write("Models/cube.gltf", kGltfWithTexture);
        write("Models/prop.glb", glb(R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":8}],"meshes":[{"primitives":[]}]})", "12345678"));
    }
};
}  // namespace

TEST("SHA-256 matches FIPS 180-4 test vectors") {
    CHECK(Sha256::hexOf("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256::hexOf("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256::hexOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    Sha256 h;  // incremental, odd chunk sizes across block boundaries
    const std::string a(1000000, 'a');
    for (size_t i = 0; i < a.size(); i += 997) h.update(a.data() + i, std::min<size_t>(997, a.size() - i));
    CHECK(h.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST("importers read real headers and reject malformed input") {
    auto run = [](const std::string& path, const std::string& bytes) {
        for (auto& i : builtinImporters()) {
            const std::string ext = fs::path(path).extension().string();
            if (i->handles(ext)) return i->import({path, bytes, Json::object()});
        }
        return ImportResult{};
    };
    ImportResult r = run("a.png", png(640, 480));
    CHECK(r.ok && r.metadata["width"].asNumber() == 640 && r.metadata["channels"].asNumber() == 4);
    std::string bad = png(4, 4);
    bad[30] ^= 1;  // flip a bit inside IHDR: CRC must catch it
    CHECK(!run("a.png", bad).ok);
    CHECK(!run("a.png", png(4, 4).substr(0, 40)).ok);         // truncated
    CHECK(!run("a.png", png(0, 4)).ok);                        // zero width
    std::string huge = png(4, 4);
    huge.replace(8, 4, be32(0x7FFFFFF0));                      // chunk length far past the end
    CHECK(!run("a.png", huge).ok);
    r = run("b.jpg", jpeg(320, 200));
    CHECK(r.ok && r.metadata["width"].asNumber() == 320 && r.metadata["height"].asNumber() == 200);
    CHECK(!run("b.jpg", std::string("\xFF\xD8\xFF\xC0\xFF\xFF", 6)).ok);  // segment length past the end
    r = run("c.wav", wav(44100, 1, 44100));
    CHECK(r.ok && r.metadata["sampleRate"].asNumber() == 44100);
    CHECK_NEAR(r.metadata["durationSeconds"].asNumber(), 1.0, 1e-9);
    std::string w = wav(44100, 1, 10);
    w.replace(40, 4, le32(0x7FFFFFFF));                         // data chunk size past the end
    CHECK(!run("c.wav", w).ok);
    r = run("d.glb", glb(R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":8}]})", "abcdefgh"));
    CHECK(r.ok && r.metadata["format"].asString() == "glb");
    CHECK(!run("d.glb", glb(R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":64}]})", "abcdefgh")).ok);
    CHECK(!run("d.glb", glb(R"({"asset":{"version":"1.0"}})", "")).ok);
    CHECK(!run("d.glb", glb("{not json", "")).ok);
    std::string lie = glb(R"({"asset":{"version":"2.0"}})", "");
    lie.replace(8, 4, le32(999999));                             // length field lies
    CHECK(!run("d.glb", lie).ok);
    CHECK(!run("e.gltf", R"({"asset":{"version":"2.0"},"buffers":[{"uri":"x.bin","byteLength":4}],
        "bufferViews":[{"buffer":0,"byteOffset":2,"byteLength":4}]})").ok);  // view outside its buffer
}

TEST("references cannot escape Assets/") {
    CHECK(resolveReference("Models/a.gltf", "b.bin") == "Models/b.bin");
    CHECK(resolveReference("Models/a.gltf", "../Textures/brick%20wall.png") == "Textures/brick wall.png");
    CHECK(resolveReference("a.gltf", "data:application/octet-stream;base64,AAAA") == "");
    CHECK_THROWS(resolveReference("Models/a.gltf", "../../secret.png"));
    CHECK_THROWS(resolveReference("a.gltf", "/etc/passwd"));
    CHECK_THROWS(resolveReference("a.gltf", "file:///etc/passwd"));
    CHECK_THROWS(resolveReference("a.gltf", "C:/x.png"));
    CHECK_THROWS(resolveReference("a.gltf", "..%2F..%2Fsecret"));
    CHECK_THROWS(resolveReference("a.gltf", "a\\b.png"));
}

TEST("first refresh imports everything, second reuses everything") {
    Project p;
    p.standardSet();
    AssetDatabase db(p.root);
    RefreshStats s = db.refresh();
    CHECK(s.scanned == 6 && s.imported == 6 && s.failed == 0 && s.reused == 0);
    const AssetRecord* tex = db.findByPath("Textures/brick wall.png");
    const AssetRecord* model = db.findByPath("Models/cube.gltf");
    CHECK(tex && model && tex->status == Status::Ok && model->status == Status::Ok);
    CHECK(model->dependencies.size() == 2);  // cube.bin + the texture
    CHECK(db.findByPath("Models/prop.glb")->metadata["meshes"].asNumber() == 1);
    CHECK(fs::exists(p.root / "Assets/Textures/brick wall.png.tmeta"));
    CHECK(fs::exists(db.artifactPath(tex->cacheKey)));
    AssetDatabase reopened(p.root);  // registry persisted
    s = reopened.refresh();
    CHECK(s.imported == 0 && s.reused == 6 && s.hashed == 0);
}

TEST("changing a source re-imports it and its dependents only") {
    Project p;
    p.standardSet();
    AssetDatabase db(p.root);
    db.refresh();
    const std::string modelKey = db.findByPath("Models/cube.gltf")->cacheKey;
    const std::string audioKey = db.findByPath("Audio/hit.wav")->cacheKey;
    p.write("Textures/brick wall.png", png(128, 128, 7));
    p.touchLater("Textures/brick wall.png");
    RefreshStats s = db.refresh();
    CHECK(s.imported == 2);  // the texture and the glTF that reads it
    CHECK(s.reused == 4);
    CHECK(db.findByPath("Textures/brick wall.png")->metadata["width"].asNumber() == 128);
    CHECK(db.findByPath("Models/cube.gltf")->cacheKey != modelKey);
    CHECK(db.findByPath("Audio/hit.wav")->cacheKey == audioKey);
    const std::string texId = db.findByPath("Textures/brick wall.png")->id;
    CHECK(db.dependents(texId).size() == 1);
}

TEST("same mtime and size skip hashing; touching without change re-hashes but reuses the product") {
    Project p;
    p.standardSet();
    AssetDatabase db(p.root);
    db.refresh();
    p.touchLater("Audio/hit.wav");
    RefreshStats s = db.refresh();
    CHECK(s.hashed == 1 && s.imported == 0 && s.reused == 6);
}

TEST("moving a source with its .tmeta keeps the id and reuses the product") {
    Project p;
    p.standardSet();
    AssetDatabase db(p.root);
    db.refresh();
    const std::string id = db.findByPath("Audio/hit.wav")->id;
    fs::create_directories(p.root / "Assets/Sfx");
    fs::rename(p.root / "Assets/Audio/hit.wav", p.root / "Assets/Sfx/impact.wav");
    fs::rename(p.root / "Assets/Audio/hit.wav.tmeta", p.root / "Assets/Sfx/impact.wav.tmeta");
    RefreshStats s = db.refresh();
    CHECK(db.findByPath("Sfx/impact.wav") && db.findByPath("Sfx/impact.wav")->id == id);
    CHECK(s.renamed == 1 && s.imported == 0 && s.missing == 0);
}

TEST("a source renamed without its .tmeta is re-associated by content hash") {
    Project p;
    p.standardSet();
    AssetDatabase db(p.root);
    db.refresh();
    const std::string id = db.findByPath("Textures/rock.jpg")->id;
    fs::rename(p.root / "Assets/Textures/rock.jpg", p.root / "Assets/Textures/granite.jpg");
    fs::remove(p.root / "Assets/Textures/rock.jpg.tmeta");
    RefreshStats s = db.refresh();
    CHECK(db.findByPath("Textures/granite.jpg")->id == id);
    CHECK(s.renamed == 1 && s.imported == 0);
}

TEST("a failed re-import keeps the previous valid product") {
    Project p;
    p.standardSet();
    AssetDatabase db(p.root);
    db.refresh();
    const AssetRecord before = *db.findByPath("Textures/rock.jpg");
    p.write("Textures/rock.jpg", "this is not a jpeg");
    p.touchLater("Textures/rock.jpg");
    RefreshStats s = db.refresh();
    const AssetRecord* after = db.findByPath("Textures/rock.jpg");
    CHECK(s.failed == 1);
    CHECK(after->status == Status::Failed && !after->error.empty());
    CHECK(after->cacheKey == before.cacheKey && after->metadata == before.metadata);
    CHECK(fs::exists(db.artifactPath(before.cacheKey)));
    p.write("Textures/rock.jpg", jpeg(16, 16));  // fixed: imported again
    p.touchLater("Textures/rock.jpg");
    s = db.refresh();
    CHECK(db.findByPath("Textures/rock.jpg")->status == Status::Ok && s.imported == 1);
}

TEST("missing and escaping dependencies fail the glTF clearly") {
    Project p;
    p.write("Models/a.gltf", R"({"asset":{"version":"2.0"},"images":[{"uri":"nothere.png"}]})");
    p.write("Models/b.gltf", R"({"asset":{"version":"2.0"},"images":[{"uri":"../../outside.png"}]})");
    AssetDatabase db(p.root);
    RefreshStats s = db.refresh();
    CHECK(s.failed == 2);
    CHECK(db.findByPath("Models/a.gltf")->error.find("missing dependency") != std::string::npos);
    CHECK(db.findByPath("Models/b.gltf")->error.find("escapes") != std::string::npos);
    p.write("Models/nothere.png", png(2, 2));  // the dependency appears: a.gltf imports on the next refresh
    s = db.refresh();
    CHECK(db.findByPath("Models/a.gltf")->status == Status::Ok);
}

TEST("a previously valid dependency that is deleted fails its dependent, which recovers when it returns") {
    Project p;
    p.standardSet();
    AssetDatabase db(p.root);
    db.refresh();
    const AssetRecord good = *db.findByPath("Models/cube.gltf");
    const std::string bin = std::string(24, 'x');
    fs::remove(p.root / "Assets/Models/cube.bin");
    db.refresh();
    const AssetRecord* gltf = db.findByPath("Models/cube.gltf");
    CHECK(gltf->status == Status::Failed);
    CHECK(gltf->error.find("missing dependency 'Models/cube.bin'") != std::string::npos);
    CHECK(gltf->cacheKey == good.cacheKey && fs::exists(db.artifactPath(good.cacheKey)));  // last good product kept
    p.write("Models/cube.bin", bin);  // reappears with the same bytes
    RefreshStats s = db.refresh();
    CHECK(db.findByPath("Models/cube.gltf")->status == Status::Ok);
    CHECK(s.failed == 0);
}

TEST("a dependency renamed with its .tmeta fails the reference that no longer resolves") {
    Project p;
    p.standardSet();
    AssetDatabase db(p.root);
    db.refresh();
    fs::rename(p.root / "Assets/Models/cube.bin", p.root / "Assets/Models/mesh.bin");
    fs::rename(p.root / "Assets/Models/cube.bin.tmeta", p.root / "Assets/Models/mesh.bin.tmeta");
    db.refresh();
    const AssetRecord* gltf = db.findByPath("Models/cube.gltf");
    CHECK(gltf->status == Status::Failed && gltf->error.find("cube.bin") != std::string::npos);
    fs::rename(p.root / "Assets/Models/mesh.bin", p.root / "Assets/Models/cube.bin");  // moved back
    fs::rename(p.root / "Assets/Models/mesh.bin.tmeta", p.root / "Assets/Models/cube.bin.tmeta");
    db.refresh();
    CHECK(db.findByPath("Models/cube.gltf")->status == Status::Ok);
}

TEST("oversized sources are refused without being read") {
    Project p;
    p.write("big.wav", wav(8000, 1, 100000));  // ~200 KB
    Options o;
    o.maxSourceBytes = 1000;
    AssetDatabase db(p.root, o);
    RefreshStats s = db.refresh();
    CHECK(s.failed == 1 && s.hashed == 0);
    CHECK(db.findByPath("big.wav")->error.find("above the limit") != std::string::npos);
}

TEST("symlinks and hidden files are never imported") {
    Project p;
    p.write("ok.png", png(2, 2));
    const fs::path outside = p.root / "outside.png";
    std::ofstream(outside, std::ios::binary) << png(3, 3);
    fs::create_symlink(outside, p.root / "Assets/link.png");
    p.write(".hidden.png", png(2, 2));
    AssetDatabase db(p.root);
    RefreshStats s = db.refresh();
    CHECK(s.scanned == 1 && db.findByPath("link.png") == nullptr && db.findByPath(".hidden.png") == nullptr);
}

TEST("cancellation saves finished work and the next refresh completes") {
    Project p;
    p.standardSet();
    AssetDatabase db(p.root);
    std::atomic<bool> cancel{true};
    RefreshStats s = db.refresh(&cancel);
    CHECK(s.cancelled && s.imported == 0);
    cancel = false;
    s = db.refresh(&cancel);
    CHECK(!s.cancelled && s.imported == 6);
}

TEST("a corrupt artifact or registry is rebuilt, never trusted") {
    Project p;
    p.standardSet();
    {
        AssetDatabase db(p.root);
        db.refresh();
        const std::string key = db.findByPath("Audio/hit.wav")->cacheKey;
        std::ofstream(db.artifactPath(key), std::ios::trunc) << "{ half-written";
        RefreshStats s = db.refresh();
        CHECK(s.imported == 1 && s.reused == 5);
    }
    const std::string id = AssetDatabase(p.root).findByPath("Audio/hit.wav")->id;
    std::ofstream(p.root / "Library/AssetDB.json", std::ios::trunc) << "garbage";
    AssetDatabase db2(p.root);
    RefreshStats s = db2.refresh();  // registry rebuilt; ids come back from the sidecars
    CHECK(db2.findByPath("Audio/hit.wav")->id == id);
    CHECK(s.failed == 0 && db2.records().size() == 6);
}

TEST("cache keys are deterministic across project copies") {
    Project a, b;
    a.standardSet();
    b.standardSet();
    AssetDatabase da(a.root), dbb(b.root);
    da.refresh();
    dbb.refresh();
    // Ids differ (random), but keys depend only on importer, settings and content.
    CHECK(da.findByPath("Textures/rock.jpg")->cacheKey == dbb.findByPath("Textures/rock.jpg")->cacheKey);
    CHECK(da.findByPath("Models/cube.gltf")->cacheKey == dbb.findByPath("Models/cube.gltf")->cacheKey);
}

TEST("deleted sources are marked missing; garbage collection removes only unreferenced artifacts") {
    Project p;
    p.standardSet();
    AssetDatabase db(p.root);
    db.refresh();
    const std::string jpgKey = db.findByPath("Textures/rock.jpg")->cacheKey;
    const std::string oldWavKey = db.findByPath("Audio/hit.wav")->cacheKey;
    fs::remove(p.root / "Assets/Textures/rock.jpg");
    p.write("Audio/hit.wav", wav(22050, 1, 100));
    p.touchLater("Audio/hit.wav");
    RefreshStats s = db.refresh();
    CHECK(s.missing == 1);
    const auto recs = db.records();
    bool sawMissing = false;
    for (const auto& r : recs) sawMissing |= r.path == "Textures/rock.jpg" && r.status == Status::Missing;
    CHECK(sawMissing);
    CHECK(db.collectGarbage() == 1);             // only the replaced wav product
    CHECK(!fs::exists(db.artifactPath(oldWavKey)));
    CHECK(fs::exists(db.artifactPath(jpgKey)));  // still referenced by the missing record (restorable)
}

TITAN_TEST_MAIN()
