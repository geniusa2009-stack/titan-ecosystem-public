// TITAN Engine — SHA-256 (FIPS 180-4), used for content hashes and deterministic cache keys.
// Original TITAN code, dependency-free.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace titan {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    void update(std::string_view s) { update(s.data(), s.size()); }
    std::array<uint8_t, 32> digest();  // finalizes; the object must not be updated afterwards
    std::string hex();                  // finalizes and returns 64 lowercase hex characters

    static std::string hexOf(std::string_view data);

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t bufLen_ = 0;
    uint64_t total_ = 0;
    bool done_ = false;
};

// Hashes a file in bounded chunks. Throws std::runtime_error if it cannot be read.
std::string sha256File(const std::filesystem::path& file);

}  // namespace titan
