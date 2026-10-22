#pragma once
// UTF-8 text and paths (C++20's u8string is its own type)
#include <filesystem>
#include <string>

namespace launcher {

inline std::string U8(const std::filesystem::path& p) {
    std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
inline std::filesystem::path PathOf(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }

}  // namespace launcher
