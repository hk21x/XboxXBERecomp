#pragma once
// Unpacking the toolchain's archives: .zip (and .vsix, a zip) with miniz, .tar.xz with xz-embedded, streamed
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace launcher {

// where an entry goes, relative to the destination ('/' separated), or empty to leave it out
using EntryFilter = std::function<std::string(const std::string& name)>;
using Progress = std::function<void(uint64_t done, uint64_t total)>;

// every entry the filter keeps, written under dest; false with the reason
bool Unzip(const std::filesystem::path& zip, const std::filesystem::path& dest, const EntryFilter& filter,
           const Progress& progress, const std::atomic<bool>* cancel, std::string& error);
bool UntarXz(const std::filesystem::path& file, const std::filesystem::path& dest, const EntryFilter& filter,
             const Progress& progress, const std::atomic<bool>* cancel, std::string& error);

// "a/b/c" without its first n components, empty when it has no more
std::string StripComponents(const std::string& name, int n);

}  // namespace launcher
