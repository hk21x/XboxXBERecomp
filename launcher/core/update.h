#pragma once
// Updates: the game's latest published release (GitHub's API on game.json's repo), and how versions compare. A
// launcher built for a release ("v1.0.2", its tag) offers that release's launcher when a newer one is out, and builds
// the game from its own tag; a launcher built from a branch ("dev") does neither.
#include <cstdint>
#include <string>

#include "core/game.h"
#include "core/net.h"

namespace launcher {

struct Release {
    std::string tag;            // "v1.0.2"
    std::string name;           // the release's title
    std::string notes;          // its notes (Markdown)
    std::string page;           // the release's web page
    std::string launcher_url;   // its <short>Launcher.exe, empty if it has none
    std::string launcher_sha256;  // the asset's SHA-256 when GitHub lists one
    uint64_t launcher_size = 0;
};

// "v1.0.2" -> true; "dev" or a branch name -> false
bool IsReleaseVersion(const std::string& version);
// < 0, 0, > 0 as a is older, the same, newer than b (release versions; anything else is older than any release)
int CompareVersions(const std::string& a, const std::string& b);
// https://github.com/owner/name(.git) -> https://api.github.com/repos/owner/name/releases/latest, empty if not GitHub
std::string LatestReleaseUrl(const std::string& repo);
// the newest published release; false (with the reason) offline, for a private repository or one with none
bool LatestRelease(Http& http, const GameConfig& game, Release& out, std::string& error);

}  // namespace launcher
