// TITAN Engine — built-in asset importers: glTF 2.0 (.gltf/.glb), PNG, JPEG, WAV. Original TITAN code.
// Every parser is bounds-checked and returns an error instead of reading past the input.
#include "titan/assets.h"
#include "titan/prefab.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <stdexcept>

namespace titan::assets {
namespace {

uint32_t be32(const std::string& b, size_t o) {
    return uint32_t(uint8_t(b[o])) << 24 | uint32_t(uint8_t(b[o + 1])) << 16 | uint32_t(uint8_t(b[o + 2])) << 8 |
           uint8_t(b[o + 3]);
}
uint16_t be16(const std::string& b, size_t o) { return uint16_t(uint8_t(b[o]) << 8 | uint8_t(b[o + 1])); }
uint32_t le32(const std::string& b, size_t o) {
    return uint32_t(uint8_t(b[o])) | uint32_t(uint8_t(b[o + 1])) << 8 | uint32_t(uint8_t(b[o + 2])) << 16 |
           uint32_t(uint8_t(b[o + 3])) << 24;
}
uint16_t le16(const std::string& b, size_t o) { return uint16_t(uint8_t(b[o]) | uint8_t(b[o + 1]) << 8); }

ImportResult fail(std::string why) {
    ImportResult r;
    r.error = std::move(why);
    return r;
}

uint32_t crc32(const char* p, size_t n) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ uint8_t(p[i])) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

constexpr uint32_t kMaxImageSide = 65536;

class PngImporter : public Importer {
public:
    std::string id() const override { return "image.png"; }
    int version() const override { return 1; }
    bool handles(const std::string& e) const override { return e == ".png"; }
    ImportResult import(const ImportContext& ctx) const override {
        const std::string& b = ctx.bytes;
        static const char sig[8] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};
        if (b.size() < 8 || std::memcmp(b.data(), sig, 8) != 0) return fail("not a PNG file (bad signature)");
        size_t o = 8;
        bool ihdr = false, idat = false, iend = false;
        uint32_t w = 0, h = 0;
        int depth = 0, colorType = -1, interlace = 0;
        while (o + 12 <= b.size()) {
            const uint32_t len = be32(b, o);
            if (len > b.size() - o - 12) return fail("PNG chunk at offset " + std::to_string(o) + " runs past the end");
            const std::string type = b.substr(o + 4, 4);
            if (crc32(b.data() + o + 4, size_t(len) + 4) != be32(b, o + 8 + len))
                return fail("PNG chunk " + type + " has a bad CRC");
            if (!ihdr && type != "IHDR") return fail("PNG does not start with IHDR");
            if (type == "IHDR") {
                if (ihdr || len != 13) return fail("bad PNG IHDR");
                w = be32(b, o + 8);
                h = be32(b, o + 12);
                depth = uint8_t(b[o + 16]);
                colorType = uint8_t(b[o + 17]);
                interlace = uint8_t(b[o + 20]);
                ihdr = true;
            } else if (type == "IDAT") {
                idat = true;
            } else if (type == "IEND") {
                iend = true;
                break;
            }
            o += 12 + size_t(len);
        }
        if (!ihdr || !idat || !iend) return fail("PNG is truncated or has no image data");
        if (w == 0 || h == 0 || w > kMaxImageSide || h > kMaxImageSide)
            return fail("PNG size " + std::to_string(w) + "x" + std::to_string(h) + " is out of range");
        int channels = 0;
        switch (colorType) {
            case 0: channels = 1; break;
            case 2: channels = 3; break;
            case 3: channels = 1; break;
            case 4: channels = 2; break;
            case 6: channels = 4; break;
            default: return fail("PNG color type " + std::to_string(colorType) + " is invalid");
        }
        if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) return fail("PNG bit depth is invalid");
        ImportResult r;
        r.ok = true;
        r.metadata["kind"] = "texture";
        r.metadata["format"] = "png";
        r.metadata["width"] = w;
        r.metadata["height"] = h;
        r.metadata["channels"] = channels;
        r.metadata["bitDepth"] = depth;
        r.metadata["palette"] = colorType == 3;
        r.metadata["interlaced"] = interlace != 0;
        return r;
    }
};

class JpegImporter : public Importer {
public:
    std::string id() const override { return "image.jpeg"; }
    int version() const override { return 1; }
    bool handles(const std::string& e) const override { return e == ".jpg" || e == ".jpeg"; }
    ImportResult import(const ImportContext& ctx) const override {
        const std::string& b = ctx.bytes;
        if (b.size() < 4 || uint8_t(b[0]) != 0xFF || uint8_t(b[1]) != 0xD8) return fail("not a JPEG file (no SOI)");
        size_t o = 2;
        while (o + 4 <= b.size()) {
            if (uint8_t(b[o]) != 0xFF) return fail("JPEG marker expected at offset " + std::to_string(o));
            const uint8_t m = uint8_t(b[o + 1]);
            if (m == 0xFF) { ++o; continue; }  // fill byte
            if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { o += 2; continue; }  // no length
            if (m == 0xD9) break;
            const uint16_t len = be16(b, o + 2);
            if (len < 2 || size_t(len) > b.size() - o - 2) return fail("JPEG segment runs past the end");
            const bool sof = m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
            if (sof) {
                if (len < 8) return fail("bad JPEG frame header");
                const uint16_t h = be16(b, o + 5), w = be16(b, o + 7);
                const int comps = uint8_t(b[o + 9]);
                if (w == 0 || h == 0) return fail("JPEG has zero size");
                if (comps < 1 || comps > 4) return fail("JPEG component count is invalid");
                ImportResult r;
                r.ok = true;
                r.metadata["kind"] = "texture";
                r.metadata["format"] = "jpeg";
                r.metadata["width"] = w;
                r.metadata["height"] = h;
                r.metadata["channels"] = comps;
                r.metadata["progressive"] = m == 0xC2;
                return r;
            }
            if (m == 0xDA) return fail("JPEG scan starts before a frame header");
            o += 2 + size_t(len);
        }
        return fail("JPEG has no frame header");
    }
};

class WavImporter : public Importer {
public:
    std::string id() const override { return "audio.wav"; }
    int version() const override { return 1; }
    bool handles(const std::string& e) const override { return e == ".wav"; }
    ImportResult import(const ImportContext& ctx) const override {
        const std::string& b = ctx.bytes;
        if (b.size() < 12 || b.compare(0, 4, "RIFF") != 0 || b.compare(8, 4, "WAVE") != 0)
            return fail("not a WAV file (no RIFF/WAVE header)");
        size_t o = 12;
        bool fmt = false;
        uint16_t format = 0, channels = 0, blockAlign = 0, bits = 0;
        uint32_t rate = 0;
        while (o + 8 <= b.size()) {
            const std::string id4 = b.substr(o, 4);
            const uint32_t len = le32(b, o + 4);
            if (len > b.size() - o - 8) return fail("WAV chunk " + id4 + " runs past the end");
            if (id4 == "fmt ") {
                if (len < 16) return fail("WAV fmt chunk is too short");
                format = le16(b, o + 8);
                channels = le16(b, o + 10);
                rate = le32(b, o + 12);
                blockAlign = le16(b, o + 20);
                bits = le16(b, o + 22);
                fmt = true;
            } else if (id4 == "data") {
                if (!fmt) return fail("WAV data chunk before fmt chunk");
                if (format != 1 && format != 3 && format != 0xFFFE)
                    return fail("WAV encoding " + std::to_string(format) + " is not supported (PCM or float only)");
                if (channels < 1 || channels > 32 || rate < 1 || rate > 768000 || bits < 8 || bits > 64 ||
                    blockAlign != channels * ((bits + 7) / 8))
                    return fail("WAV format fields are inconsistent");
                const uint64_t frames = len / blockAlign;
                ImportResult r;
                r.ok = true;
                r.metadata["kind"] = "audio";
                r.metadata["format"] = "wav";
                r.metadata["channels"] = channels;
                r.metadata["sampleRate"] = rate;
                r.metadata["bitsPerSample"] = bits;
                r.metadata["frames"] = double(frames);
                r.metadata["durationSeconds"] = double(frames) / double(rate);
                return r;
            }
            o += 8 + size_t(len) + (len & 1);  // chunks are padded to even sizes
        }
        return fail("WAV has no data chunk");
    }
};

class GltfImporter : public Importer {
public:
    std::string id() const override { return "model.gltf"; }
    int version() const override { return 1; }
    bool handles(const std::string& e) const override { return e == ".gltf" || e == ".glb"; }
    ImportResult import(const ImportContext& ctx) const override {
        const std::string& b = ctx.bytes;
        std::string jsonText;
        uint64_t binLength = 0;
        bool glb = b.size() >= 4 && b.compare(0, 4, "glTF") == 0;
        if (glb) {
            if (b.size() < 20) return fail("GLB header is truncated");
            if (le32(b, 4) != 2) return fail("GLB version " + std::to_string(le32(b, 4)) + " is not 2");
            if (le32(b, 8) != b.size()) return fail("GLB length field does not match the file size");
            const uint32_t jsonLen = le32(b, 12);
            if (le32(b, 16) != 0x4E4F534Au) return fail("GLB first chunk is not JSON");
            if (jsonLen > b.size() - 20) return fail("GLB JSON chunk runs past the end");
            jsonText = b.substr(20, jsonLen);
            const size_t next = 20 + size_t(jsonLen);
            if (next + 8 <= b.size()) {
                const uint32_t binLen = le32(b, next);
                if (le32(b, next + 4) == 0x004E4942u) {
                    if (binLen > b.size() - next - 8) return fail("GLB BIN chunk runs past the end");
                    binLength = binLen;
                }
            }
        } else {
            jsonText = b;
        }
        Json doc;
        try {
            doc = Json::parse(jsonText, 64);
        } catch (const std::exception& e) {
            return fail(std::string("glTF JSON is invalid: ") + e.what());
        }
        if (!doc.isObject() || !doc["asset"].isObject() || doc["asset"].getString("version").rfind("2", 0) != 0)
            return fail("not a glTF 2.0 document (asset.version)");
        ImportResult r;
        try {
            auto count = [&](const char* key) -> double {
                const Json& a = doc[key];
                if (a.isNull()) return 0;
                if (!a.isArray()) throw std::runtime_error(std::string("glTF '") + key + "' is not an array");
                return double(a.size());
            };
            std::vector<uint64_t> bufferLengths;
            const Json& buffers = doc["buffers"];
            for (size_t i = 0; buffers.isArray() && i < buffers.size(); ++i) {
                const Json& buf = buffers.at(i);
                const double len = buf.getNumber("byteLength", -1);
                if (len < 0) throw std::runtime_error("glTF buffer " + std::to_string(i) + " has no byteLength");
                bufferLengths.push_back(uint64_t(len));
                if (buf.contains("uri")) {
                    const std::string dep = resolveReference(ctx.path, buf.getString("uri"));
                    if (!dep.empty()) r.dependencyPaths.push_back(dep);
                } else if (!glb || i != 0 || uint64_t(len) > binLength) {
                    throw std::runtime_error("glTF buffer " + std::to_string(i) + " has no data (no uri, no GLB BIN)");
                }
            }
            const Json& views = doc["bufferViews"];
            for (size_t i = 0; views.isArray() && i < views.size(); ++i) {
                const Json& v = views.at(i);
                const double bi = v.getNumber("buffer", -1), off = v.getNumber("byteOffset", 0),
                             len = v.getNumber("byteLength", -1);
                if (bi < 0 || bi >= double(bufferLengths.size()) || off < 0 || len < 0 ||
                    off + len > double(bufferLengths[size_t(bi)]))
                    throw std::runtime_error("glTF bufferView " + std::to_string(i) + " is outside its buffer");
            }
            const Json& images = doc["images"];
            for (size_t i = 0; images.isArray() && i < images.size(); ++i)
                if (images.at(i).contains("uri")) {
                    const std::string dep = resolveReference(ctx.path, images.at(i).getString("uri"));
                    if (!dep.empty()) r.dependencyPaths.push_back(dep);
                }
            r.metadata["kind"] = "model";
            r.metadata["format"] = glb ? "glb" : "gltf";
            r.metadata["meshes"] = count("meshes");
            r.metadata["nodes"] = count("nodes");
            r.metadata["materials"] = count("materials");
            r.metadata["textures"] = count("textures");
            r.metadata["images"] = count("images");
            r.metadata["animations"] = count("animations");
            r.metadata["generator"] = doc["asset"].getString("generator");
        } catch (const std::exception& e) {
            return fail(e.what());
        }
        r.ok = true;
        return r;
    }
};

// Raw binary buffers (glTF .bin): tracked so that changing one re-imports the models that read it.
class BufferImporter : public Importer {
public:
    std::string id() const override { return "data.buffer"; }
    int version() const override { return 1; }
    bool handles(const std::string& e) const override { return e == ".bin"; }
    ImportResult import(const ImportContext& ctx) const override {
        ImportResult r;
        r.ok = true;
        r.metadata["kind"] = "buffer";
        r.metadata["bytes"] = double(ctx.bytes.size());
        return r;
    }
};

// TITAN prefabs (.tprefab): validated with the same rules the engine uses to instantiate them.
class PrefabImporter : public Importer {
public:
    std::string id() const override { return "titan.prefab"; }
    int version() const override { return 1; }
    bool handles(const std::string& e) const override { return e == ".tprefab"; }
    ImportResult import(const ImportContext& ctx) const override {
        try {
            const Prefab p = Prefab::fromJson(Json::parse(ctx.bytes, 64));
            ImportResult r;
            r.ok = true;
            r.metadata["kind"] = "prefab";
            r.metadata["prefabId"] = p.id;
            r.metadata["name"] = p.name;
            r.metadata["objects"] = double(p.objects.size());
            return r;
        } catch (const std::exception& e) {
            return fail(std::string("invalid prefab: ") + e.what());
        }
    }
};

std::string percentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(uint8_t(s[i + 1])) && std::isxdigit(uint8_t(s[i + 2]))) {
            out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

}  // namespace

std::string resolveReference(const std::string& fromPath, const std::string& uri) {
    if (uri.rfind("data:", 0) == 0) return "";
    if (uri.empty()) throw std::runtime_error("empty reference");
    if (uri.find("://") != std::string::npos || uri[0] == '/' || uri[0] == '\\' || uri.find(':') != std::string::npos)
        throw std::runtime_error("reference '" + uri + "' is not a relative path inside Assets/");
    const std::string decoded = percentDecode(uri);
    if (decoded.find('\\') != std::string::npos || decoded.find('\0') != std::string::npos)
        throw std::runtime_error("reference '" + uri + "' contains a backslash or NUL");
    std::vector<std::string> parts;
    const size_t slash = fromPath.rfind('/');
    const std::string base = slash == std::string::npos ? "" : fromPath.substr(0, slash) + "/";
    const std::string joined = base + decoded;
    size_t start = 0;
    while (start <= joined.size()) {
        const size_t end = std::min(joined.find('/', start), joined.size());
        const std::string seg = joined.substr(start, end - start);
        if (seg == "..") {
            if (parts.empty()) throw std::runtime_error("reference '" + uri + "' escapes Assets/");
            parts.pop_back();
        } else if (!seg.empty() && seg != ".") {
            parts.push_back(seg);
        }
        start = end + 1;
    }
    if (parts.empty()) throw std::runtime_error("reference '" + uri + "' names no file");
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : "/") + p;
    return out;
}

std::vector<std::unique_ptr<Importer>> builtinImporters() {
    std::vector<std::unique_ptr<Importer>> v;
    v.push_back(std::make_unique<GltfImporter>());
    v.push_back(std::make_unique<PngImporter>());
    v.push_back(std::make_unique<JpegImporter>());
    v.push_back(std::make_unique<WavImporter>());
    v.push_back(std::make_unique<BufferImporter>());
    v.push_back(std::make_unique<PrefabImporter>());
    return v;
}

}  // namespace titan::assets
