#pragma once
// What the launcher puts in its tools folder: the toolchain.json a game's repository publishes (clang, CMake, Ninja,
// MinGit, each pinned with its SHA-256) and Microsoft's own packages for the C++ libraries and the Windows SDK,
// resolved from the Visual Studio manifest (each file there carries its SHA-256 too). Nothing is installed: zips and
// archives are unpacked, the SDK's .msi files extracted (msiexec /a, no rights needed).
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/net.h"

namespace launcher {

struct FileRef {
    std::string name;  // the local file name
    std::string url, sha256;
    uint64_t size = 0;
};

struct Tool {
    std::string id;     // folder under tools\ ("llvm", "msvc", ...)
    std::string title;  // for the window ("Clang 19.1.7")
    std::string kind;   // "zip", "tar.xz", "vsix" (zips with Contents/, names URL-encoded), "msi"
    std::string stamp;  // written once unpacked; another value unpacks again
    std::vector<FileRef> files;
    std::vector<FileRef> cabs;  // msi: the package's .cab files, the ones an .msi names are fetched with it
    int strip = 0;               // leading path components dropped
    std::vector<std::string> keep;  // after strip: "dir/" a folder and all in it, else a path ("*" within a name); empty: all
    bool optional = false;          // not part of the build: fetched when a feature asks (ffmpeg, for the music)
    uint64_t Bytes() const;
};

struct Plan {
    std::vector<Tool> tools;
    std::string sdk_version;  // "10.0.26100.0"
};

// toolchain.json: {"tools": [...], "msvc": {...}, "sdk": {...}}; false with the reason
bool LoadPlan(const std::string& toolchain_json, Plan& plan, std::string& error);
// the Microsoft parts: the channel's manifest fetched and the msvc and sdk entries of toolchain.json turned into
// tools with their files
bool ResolveMicrosoft(Http& http, const std::string& toolchain_json, Plan& plan, std::string& error);

// keep and strip applied to an archive entry's name
std::string Select(const Tool& tool, const std::string& name);
std::string UrlDecode(const std::string& s);
// the .cab names an .msi refers to (they sit beside it), from its bytes
std::vector<std::string> MsiCabs(const std::vector<uint8_t>& msi);

}  // namespace launcher
