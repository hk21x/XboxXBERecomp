#pragma once
// SHA-1 (the game's xbe, as the runtime names releases) and SHA-256 (every download), incremental
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace launcher {

class Sha1 {
public:
    Sha1();
    void Update(const void* data, size_t len);
    std::string Hex();  // lower case, ends the hash

private:
    void Block(const uint8_t* p);
    uint32_t h_[5];
    uint8_t buf_[64];
    uint64_t len_ = 0;
};

class Sha256 {
public:
    Sha256();
    void Update(const void* data, size_t len);
    std::string Hex();

private:
    void Block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    uint64_t len_ = 0;
};

// a file's SHA-256, empty when it cannot be read
std::string FileSha256(const std::filesystem::path& path);

}  // namespace launcher
