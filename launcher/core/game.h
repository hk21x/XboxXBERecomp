#pragma once
// The game a launcher is made for (its game.json, built into the launcher) and the check of a player's copy
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace launcher {

struct GameConfig {
    std::string title;       // "Project Gotham Racing 2"
    std::string short_name;  // "PGR2": the folder the launcher works in, and the window
    std::string exe;         // the built exe's name, without .exe
    std::string xbe;         // "default.xbe"
    std::map<std::string, std::string> releases;  // xbe SHA-1 -> release name; a copy must be one of them
    std::string repo, branch;  // the game's source (git)
    // the launcher's own version, set by the launcher (not game.json): a release's tag ("v1.0.2"), whose source it
    // builds, or "dev" for one built from a branch, which builds the branch
    std::string version = "dev";
    std::string toolchain;     // toolchain.json's address
    bool music = false;        // the game plays Xbox custom soundtracks: the launcher makes them from a music folder
    // a game of several programs (007: Everything or Nothing's driving.xbe): every xbe the build recompiles, taken from
    // the player's copy into the source's assets, and every exe the build makes that the game runs, installed beside
    // the first; game.json "xbes" and "exes", by default only xbe and exe
    std::vector<std::string> xbes, exes;
};

bool ParseGameConfig(const std::string& json_text, GameConfig& game, std::string& error);

struct GameCheck {
    bool ok = false;
    bool iso = false;     // a disc image, else a folder of the disc's files
    std::string release;  // which one ("PAL (Europe)")
    std::string sha1;
    std::string error;    // why not, for the player
    std::filesystem::path path;  // the image, or the folder holding the xbe
};

// path: an .iso, a folder with the disc's files, or the xbe in such a folder
GameCheck CheckGame(const GameConfig& game, const std::filesystem::path& path);
// the xbe's bytes from a checked copy
bool ReadXbe(const GameConfig& game, const GameCheck& copy, std::vector<uint8_t>& out, std::string& error);
// another xbe of the copy by its name (GameConfig::xbes)
bool ReadXbeNamed(const std::string& name, const GameCheck& copy, std::vector<uint8_t>& out, std::string& error);

}  // namespace launcher
