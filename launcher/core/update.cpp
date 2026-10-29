#include "core/update.h"

#include <algorithm>
#include <cctype>
#include <vector>

#include <nlohmann/json.hpp>

namespace launcher {

using json = nlohmann::json;

namespace {

// "v1.0.2" -> {1, 0, 2}; empty if not a release version
std::vector<uint32_t> Parts(const std::string& v) {
    std::vector<uint32_t> parts;
    if (v.size() < 2 || (v[0] != 'v' && v[0] != 'V') || !std::isdigit(static_cast<unsigned char>(v[1]))) return {};
    uint32_t n = 0;
    bool digits = false;
    for (size_t i = 1; i <= v.size(); i++) {
        char c = i < v.size() ? v[i] : '.';
        if (std::isdigit(static_cast<unsigned char>(c))) {
            n = n * 10 + uint32_t(c - '0');
            digits = true;
        } else if (c == '.' && digits) {
            parts.push_back(n);
            n = 0;
            digits = false;
        } else {
            return {};  // "v1.0.2-rc1" and the like are not offered
        }
    }
    return parts;
}

// a string field, empty when missing or null (GitHub sends null for an absent body or digest)
std::string Str(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

}  // namespace

bool IsReleaseVersion(const std::string& version) { return !Parts(version).empty(); }

int CompareVersions(const std::string& a, const std::string& b) {
    std::vector<uint32_t> x = Parts(a), y = Parts(b);
    if (x.empty() || y.empty()) return int(!x.empty()) - int(!y.empty());
    for (size_t i = 0; i < std::max(x.size(), y.size()); i++) {
        uint32_t p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

std::string LatestReleaseUrl(const std::string& repo) {
    const std::string host = "https://github.com/";
    if (repo.compare(0, host.size(), host) != 0) return {};
    std::string path = repo.substr(host.size());
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".git") == 0) path.resize(path.size() - 4);
    while (!path.empty() && path.back() == '/') path.pop_back();
    if (path.find('/') == std::string::npos) return {};
    return "https://api.github.com/repos/" + path + "/releases/latest";
}

bool LatestRelease(Http& http, const GameConfig& game, Release& out, std::string& error) {
    std::string url = LatestReleaseUrl(game.repo), text;
    if (url.empty()) {
        error = "the game's repository is not on GitHub";
        return false;
    }
    if (!FetchText(http, url, text, error)) return false;
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("tag_name")) {
        error = "no published release";
        return false;
    }
    out = {};
    out.tag = Str(j, "tag_name");
    out.name = Str(j, "name");
    out.notes = Str(j, "body");
    out.page = Str(j, "html_url");
    const std::string exe = game.short_name + "Launcher.exe";
    auto assets = j.find("assets");
    for (const json& a : assets != j.end() && assets->is_array() ? *assets : json::array()) {
        if (!a.is_object() || Str(a, "name") != exe) continue;
        out.launcher_url = Str(a, "browser_download_url");
        out.launcher_size = a.contains("size") && a["size"].is_number_unsigned() ? a["size"].get<uint64_t>() : 0;
        std::string digest = Str(a, "digest");  // "sha256:..." where GitHub lists it
        if (digest.compare(0, 7, "sha256:") == 0) out.launcher_sha256 = digest.substr(7);
    }
    return !out.tag.empty();
}

}  // namespace launcher
