// titan-asset-bench — measures the asset pipeline (Issue #5 M1): cold import, no-change refresh and a
// single-source change on a generated project. Original TITAN code.
// usage: titan-asset-bench [--assets N] [--runs R] [--out report.json]
// The project is generated in the system temp directory and removed afterwards.
#include "titan/assets.h"
#include "titan/project.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

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
std::string chunk(const std::string& t, const std::string& d) { return be32(uint32_t(d.size())) + t + d + be32(crc(t + d)); }
std::string png(uint32_t w, uint32_t h, size_t payload, char fill) {
    return std::string("\x89PNG\r\n\x1a\n", 8) + chunk("IHDR", be32(w) + be32(h) + std::string{8, 6, 0, 0, 0}) +
           chunk("IDAT", std::string(payload, fill)) + chunk("IEND", "");
}
double ms(std::chrono::steady_clock::time_point a) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
}
}  // namespace

int main(int argc, char** argv) {
    int n = 2000, runs = 3;
    std::string out;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--assets") n = std::stoi(argv[i + 1]);
        else if (a == "--runs") runs = std::stoi(argv[i + 1]);
        else if (a == "--out") out = argv[i + 1];
    }
    Json report = Json::object();
    report["assets"] = n;
    Json arr = Json::array();
    for (int run = 0; run < runs; ++run) {
        const fs::path root = fs::temp_directory_path() / ("titan-asset-bench-" + std::to_string(getpid()) + "-" + std::to_string(run));
        fs::remove_all(root);
        uint64_t bytes = 0;
        for (int i = 0; i < n; ++i) {  // 64 KiB textures in 20 folders
            const fs::path p = root / "Assets" / ("Folder" + std::to_string(i % 20)) / ("tex" + std::to_string(i) + ".png");
            fs::create_directories(p.parent_path());
            const std::string b = png(256, 64, 65536, char(i));
            std::ofstream(p, std::ios::binary) << b;
            bytes += b.size();
        }
        Json r = Json::object();
        auto t = std::chrono::steady_clock::now();
        AssetDatabase db(root);
        RefreshStats s = db.refresh();
        r["coldMs"] = ms(t);
        r["coldImported"] = s.imported;
        t = std::chrono::steady_clock::now();
        AssetDatabase db2(root);  // fresh process state: registry loaded from disk
        s = db2.refresh();
        r["noChangeMs"] = ms(t);
        r["noChangeReused"] = s.reused;
        r["noChangeHashed"] = s.hashed;
        const fs::path one = root / "Assets/Folder3/tex3.png";
        std::ofstream(one, std::ios::binary | std::ios::trunc) << png(512, 512, 1000, 'z');
        fs::last_write_time(one, fs::last_write_time(one) + std::chrono::seconds(2));
        t = std::chrono::steady_clock::now();
        s = db2.refresh();
        r["oneChangeMs"] = ms(t);
        r["oneChangeImported"] = s.imported;
        r["sourceBytes"] = double(bytes);
        arr.push_back(r);
        std::printf("run %d: cold %.0f ms (%d imported), no-change %.0f ms (%d reused, %d hashed), one change %.0f ms (%d imported)\n",
                    run + 1, r["coldMs"].asNumber(), int(r["coldImported"].asNumber()), r["noChangeMs"].asNumber(),
                    int(r["noChangeReused"].asNumber()), int(r["noChangeHashed"].asNumber()), r["oneChangeMs"].asNumber(),
                    int(r["oneChangeImported"].asNumber()));
        fs::remove_all(root);
    }
    report["runs"] = arr;
    if (!out.empty()) writeFileAtomic(out, report.dump(2) + "\n");
    return 0;
}
