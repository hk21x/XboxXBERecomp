#include "core/game.h"

#include <cctype>
#include <fstream>
#include <iterator>

#include "core/hash.h"
#include "kernel/disc.h"
#include "nlohmann/json.hpp"

#include "core/util.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace launcher {

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// a file in a folder whatever its case (copies from discs are often upper case)
fs::path FindAnyCase(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    if (fs::exists(dir / name, ec)) return dir / name;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (Lower(U8(e.path().filename())) == Lower(name)) return e.path();
    return {};
}

bool ReadFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

}  // namespace

bool ParseGameConfig(const std::string& json_text, GameConfig& game, std::string& error) {
    json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded()) {
        error = "game.json is not readable";
        return false;
    }
    game.title = j.value("title", "");
    game.short_name = j.value("short", "");
    game.exe = j.value("exe", game.short_name);
    game.xbe = j.value("xbe", "default.xbe");
    game.repo = j.value("repo", "");
    game.branch = j.value("branch", "main");
    game.toolchain = j.value("toolchain", "");
    json releases = j.value("releases", json::object());
    for (auto& [sha, name] : releases.items()) game.releases[Lower(sha)] = name.get<std::string>();
    if (game.title.empty() || game.short_name.empty() || game.repo.empty() || game.releases.empty()) {
        error = "game.json needs a title, a short name, a repo and its releases";
        return false;
    }
    return true;
}

bool ReadXbe(const GameConfig& game, const GameCheck& copy, std::vector<uint8_t>& out, std::string& error) {
    if (copy.iso) {
        kernel::Disc disc;
        if (!disc.Open(copy.path, error)) return false;
        if (!disc.Load(game.xbe, out)) {
            error = "the disc image has no " + game.xbe;
            return false;
        }
        return true;
    }
    fs::path xbe = FindAnyCase(copy.path, game.xbe);
    if (xbe.empty() || !ReadFile(xbe, out)) {
        error = "there is no " + game.xbe + " in " + U8(copy.path);
        return false;
    }
    return true;
}

GameCheck CheckGame(const GameConfig& game, const fs::path& given) {
    GameCheck c;
    std::error_code ec;
    fs::path path = fs::absolute(given, ec);
    if (fs::is_directory(path, ec)) {
        c.path = path;
    } else if (Lower(U8(path.extension())) == ".iso") {
        c.iso = true;
        c.path = path;
    } else if (Lower(U8(path.filename())) == Lower(game.xbe)) {
        c.path = path.parent_path();
    } else {
        c.error = "Choose the game's disc image (.iso), or the folder with its files (" + game.xbe + ").";
        return c;
    }
    std::vector<uint8_t> xbe;
    if (!ReadXbe(game, c, xbe, c.error)) {
        if (c.iso) c.error = "This is not an Xbox disc image of " + game.title + ": " + c.error;
        return c;
    }
    Sha1 h;
    h.Update(xbe.data(), xbe.size());
    c.sha1 = h.Hex();
    auto it = game.releases.find(c.sha1);
    if (it == game.releases.end()) {
        c.error = "This copy's " + game.xbe + " is not one this port supports (SHA-1 " + c.sha1 +
                  "). It needs an unmodified disc of " + game.title + ".";
        return c;
    }
    if (!c.iso) {
        // the folder must hold the disc's other files too, not the xbe alone
        size_t entries = 0;
        for (auto it2 = fs::directory_iterator(c.path, ec); it2 != fs::directory_iterator(); it2.increment(ec)) entries++;
        if (entries < 3) {
            c.error = "The folder holds " + game.xbe + " but not the rest of the disc's files.";
            return c;
        }
    }
    c.release = it->second;
    c.ok = true;
    return c;
}

}  // namespace launcher
