// launcher_cli: the launcher's steps from a terminal, to test them on Linux
//   launcher_cli <game.json> <root> plan               the tools and their downloads (fetches the manifests)
//   launcher_cli <game.json> <root> tool <id>          download and unpack one tool (not "winsdk" here)
//   launcher_cli <game.json> <root> game <iso|folder>  check a copy and link it into play/assets
//   launcher_cli <game.json> <root> source             clone or update the source, put the xbe in it
//   launcher_cli <game.json> <root> env                the build's environment
//   launcher_cli <game.json> <root> latest [version]   the newest release, and whether a launcher of version is behind
//   launcher_cli hash <file>                           SHA-256 and SHA-1
//   launcher_cli untar <file.tar.xz> <dest> [keep...]  unpack with keep rules (strip 1)
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

#include "core/builder.h"
#include "core/hash.h"
#include "core/update.h"

using namespace launcher;
namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "hash") {
        std::ifstream in(argv[2], std::ios::binary);
        std::vector<char> buf(1 << 20);
        Sha1 s1;
        Sha256 s2;
        while (in) {
            in.read(buf.data(), std::streamsize(buf.size()));
            s1.Update(buf.data(), size_t(in.gcount()));
            s2.Update(buf.data(), size_t(in.gcount()));
        }
        std::printf("sha256 %s\nsha1   %s\n", s2.Hex().c_str(), s1.Hex().c_str());
        return 0;
    }
    if (argc >= 4 && std::string(argv[1]) == "untar") {
        Tool t;
        t.kind = "tar.xz";
        t.strip = 1;
        for (int i = 4; i < argc; i++) t.keep.push_back(argv[i]);
        std::string error;
        size_t files = 0;
        bool ok = UntarXz(argv[2], argv[3], [&](const std::string& n) {
                std::string r = Select(t, n);
                if (!r.empty()) files++;
                return r;
            }, nullptr, nullptr, error);
        std::printf("%s: %zu files%s%s\n", ok ? "ok" : "failed", files, ok ? "" : ", ", error.c_str());
        return ok ? 0 : 1;
    }
    if (argc < 4) {
        std::fprintf(stderr, "usage: launcher_cli <game.json> <root> plan|tool <id>|game <path>|source|env|music <folder>\n");
        return 2;
    }
    std::ifstream gj(argv[1]);
    std::string text((std::istreambuf_iterator<char>(gj)), {}), error;
    GameConfig game;
    if (!ParseGameConfig(text, game, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    // a local toolchain.json beside game.json stands in for the published one
    fs::path local = fs::path(argv[1]).parent_path() / "toolchain.json";
    if (fs::exists(local)) game.toolchain = "file://" + fs::absolute(local).string();
    // LAUNCHER_VERSION: the version this run acts as (a release tag builds that tag; default dev, the branch)
    if (const char* v = std::getenv("LAUNCHER_VERSION")) game.version = v;
    auto http = MakeHttp();
    auto platform = MakePlatform();
    Builder b(game, argv[2], *http, *platform);
    std::string last;
    b.on_status = [&](const Status& s) {
        std::string l = s.step + (s.detail.empty() ? "" : ": " + s.detail);
        if (s.fraction >= 0) l += " (" + std::to_string(int(s.fraction * 100)) + "%)";
        if (l != last) std::printf("  %s\n", l.c_str());
        last = l;
    };
    b.on_log = [](const std::string& l) { std::printf("log: %s\n", l.c_str()); };
    std::string cmd = argv[3];
    if (cmd == "latest") {
        Release r;
        if (!LatestRelease(*http, game, r, error)) {
            std::fprintf(stderr, "latest: %s\n", error.c_str());
            return 1;
        }
        std::printf("%s \"%s\" %s\n  launcher %s (%llu bytes, sha256 %s)\n", r.tag.c_str(), r.name.c_str(), r.page.c_str(),
                    r.launcher_url.empty() ? "none" : r.launcher_url.c_str(), (unsigned long long)r.launcher_size,
                    r.launcher_sha256.empty() ? "not listed" : r.launcher_sha256.c_str());
        if (argc > 4)
            std::printf("  a launcher of %s: %s\n", argv[4],
                        !IsReleaseVersion(argv[4]) ? "dev, offered nothing"
                        : CompareVersions(r.tag, argv[4]) > 0 ? "behind: offered the update" : "up to date");
        return 0;
    }
    Plan plan;
    if (cmd == "plan" || cmd == "tool" || cmd == "env") {
        if (!b.LoadToolchain(plan, error)) {
            std::fprintf(stderr, "plan: %s\n", error.c_str());
            return 1;
        }
    }
    if (cmd == "plan") {
        for (const Tool& t : plan.tools) {
            std::printf("%-8s %-36s %-7s %3zu files %3zu cabs %s\n", t.id.c_str(), t.title.c_str(), t.kind.c_str(),
                        t.files.size(), t.cabs.size(), Megabytes(t.Bytes()).c_str());
            for (const FileRef& f : t.files) std::printf("    %s %s %s\n", f.name.c_str(), Megabytes(f.size).c_str(), f.sha256.substr(0, 16).c_str());
        }
        return 0;
    }
    if (cmd == "tool" && argc >= 5) {
        for (const Tool& t : plan.tools)
            if (t.id == argv[4]) {
                bool ok = b.InstallTool(t, error);
                std::printf("%s %s\n", ok ? "installed" : "failed:", error.c_str());
                return ok ? 0 : 1;
            }
        std::fprintf(stderr, "no tool %s\n", argv[4]);
        return 1;
    }
    if (cmd == "env") {
        for (const auto& [k, v] : b.BuildEnv(plan)) std::printf("%s=%s\n", k.c_str(), v.c_str());
        return 0;
    }
    if (cmd == "game" && argc >= 5) {
        GameCheck c = CheckGame(game, argv[4]);
        std::printf("%s: %s %s %s\n", c.ok ? "ok" : "no", c.release.c_str(), c.sha1.c_str(), c.error.c_str());
        if (!c.ok) return 1;
        bool ok = b.SetGame(c, error);
        std::printf("set: %s %s\n", ok ? "ok" : "failed", error.c_str());
        return ok ? 0 : 1;
    }
    if (cmd == "source") {
        bool ok = b.GetSource(error) && b.PutXbe(error);
        std::printf("%s %s\n", ok ? "source ready" : "failed:", error.c_str());
        return ok ? 0 : 1;
    }
    if (cmd == "music" && argc >= 5) {  // the soundtracks from a music folder, with the ffmpeg on the PATH
        std::string summary;
        bool ok = b.ImportMusic(argv[4], summary, error);
        std::printf("%s %s -> %s\n", ok ? "music:" : "failed:", ok ? summary.c_str() : error.c_str(),
                    b.MusicDir().string().c_str());
        return ok ? 0 : 1;
    }
    std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
}
