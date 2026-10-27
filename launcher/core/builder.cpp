#include "core/builder.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

#include "core/archive.h"
#include "core/hash.h"
#include "core/music.h"
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

bool ReadAll(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

// the first folder in dir (a version folder), empty if none
fs::path FirstDir(const fs::path& dir) {
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_directory(ec)) dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    return dirs.empty() ? fs::path() : dirs.back();
}

}  // namespace

std::string Megabytes(uint64_t bytes) {
    char s[32];
    if (bytes >= (uint64_t(1) << 30)) std::snprintf(s, sizeof(s), "%.1f GB", double(bytes) / double(1 << 30));
    else std::snprintf(s, sizeof(s), "%.0f MB", double(bytes) / double(1 << 20));
    return s;
}

Builder::Builder(const GameConfig& game, const fs::path& root, Http& http, Platform& platform)
    : game_(game), root_(root), http_(http), platform_(platform) {}

void Builder::Log(const std::string& line) {
    std::lock_guard lock(log_lock_);
    std::error_code ec;
    fs::create_directories(root_, ec);
    std::ofstream(root_ / "launcher.log", std::ios::app) << line << "\n";
    if (on_log) on_log(line);
}

void Builder::Say(const std::string& step, const std::string& detail, double fraction) {
    if (on_status) on_status({step, detail, fraction});
}

fs::path Builder::GameExe() const { return PlayDir() / (game_.exe + platform_.ExeSuffix()); }

bool Builder::Built() const {
    std::error_code ec;
    return fs::exists(GameExe(), ec);
}

fs::path Builder::SavedGame() const {
    std::string text;
    if (!ReadAll(root_ / "launcher.json", text)) return {};
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) return {};
    return PathOf(j.value("game", ""));
}

bool Builder::SetGame(const GameCheck& copy, std::string& error) {
    std::error_code ec;
    fs::create_directories(PlayDir(), ec);
    fs::path assets = PlayDir() / "assets";
    // the last copy's link: a junction or an empty folder goes with remove; a folder holding the image we put there
    // is emptied of images first. Never a recursive remove: a junction's target is the player's own files.
    if (fs::exists(fs::symlink_status(assets, ec)) && !fs::remove(assets, ec)) {
        for (const auto& e : fs::directory_iterator(assets, ec))
            if (Lower(U8(e.path().extension())) == ".iso") fs::remove(e.path(), ec);
        fs::remove(assets, ec);
    }
    if (fs::exists(fs::symlink_status(assets, ec))) {
        error = "Cannot replace " + U8(assets) + ": remove it by hand and choose the game again.";
        return false;
    }
    if (copy.iso) {
        fs::create_directories(assets, ec);
        fs::path to = assets / copy.path.filename();
        if (!platform_.LinkFile(to, copy.path)) {
            // another drive: a copy, with its progress
            Log("copying " + U8(copy.path) + " to " + U8(to));
            std::ifstream in(copy.path, std::ios::binary);
            std::ofstream out(to, std::ios::binary);
            uint64_t total = fs::file_size(copy.path, ec), done = 0;
            std::vector<char> buf(8 << 20);
            while (in && out) {
                in.read(buf.data(), std::streamsize(buf.size()));
                out.write(buf.data(), in.gcount());
                done += uint64_t(in.gcount());
                Say("Copying the disc image", Megabytes(done) + " of " + Megabytes(total), total ? double(done) / double(total) : -1);
                if (cancel) break;
            }
            if (!out || done != total) {
                out.close();
                fs::remove(to, ec);
                error = cancel ? "cancelled" : "Could not copy the disc image (is the drive full?)";
                return false;
            }
        }
    } else if (!platform_.LinkFolder(assets, copy.path, error)) {
        return false;
    }
    Remember("game", U8(copy.path));
    Log("game: " + U8(copy.path) + ", " + copy.release + ", sha1 " + copy.sha1);
    return true;
}

bool Builder::LoadToolchain(Plan& plan, std::string& error) {
    Say("Looking up the build tools");
    std::string text;
    fs::path saved = root_ / "toolchain.json";
    if (FetchText(http_, game_.toolchain, text, error)) {
        std::ofstream(saved, std::ios::binary) << text;
    } else if (ReadAll(saved, text)) {
        Log("toolchain.json could not be fetched (" + error + "), the last one is used");
    } else if (!fallback_toolchain.empty()) {
        Log("toolchain.json could not be fetched (" + error + "), the launcher's own is used");
        text = fallback_toolchain;
    } else {
        error = "Could not download the list of build tools: " + error;
        return false;
    }
    plan = {};
    if (!LoadPlan(text, plan, error)) return false;
    if (!ResolveMicrosoft(http_, text, plan, error)) return false;
    uint64_t bytes = 0;
    for (const Tool& t : plan.tools)
        if (!t.optional) bytes += t.Bytes();
    Log("tools: " + std::to_string(plan.tools.size()) + ", " + Megabytes(bytes) + " to download at most");
    return true;
}

bool Builder::InstallTool(const Tool& tool, std::string& error) {
    fs::path dir = ToolPath(tool.id), stamp = dir / ".launcher-stamp";
    std::string have;
    if (ReadAll(stamp, have) && have == tool.stamp) return true;
    std::error_code ec;
    fs::remove_all(dir, ec);  // our own folder under tools
    fs::path downloads = root_ / "downloads" / tool.id;
    fs::create_directories(downloads, ec);
    uint64_t total = tool.Bytes(), before = 0;
    std::string step = "Downloading " + tool.title;
    auto fetch = [&](const FileRef& f) {
        Log("download " + f.url);
        bool ok = Download(http_, f.url, downloads / f.name, f.sha256, f.size, [&](uint64_t done, uint64_t) {
                Say(step, Megabytes(before + done) + " of " + Megabytes(total), total ? double(before + done) / double(total) : -1);
            }, &cancel, error);
        before += f.size;
        if (!ok) error = "Could not download " + f.name + ": " + error;
        return ok;
    };
    for (const FileRef& f : tool.files)
        if (!fetch(f)) return false;
    Say("Unpacking " + tool.title);
    auto progress = [&](uint64_t done, uint64_t all) {
        Say("Unpacking " + tool.title, {}, all ? double(done) / double(all) : -1);
    };
    auto select = [&](const std::string& name) { return Select(tool, name); };
    if (tool.kind == "msi") {
        // each installer's cabinets beside it, then extracted into dir
        std::map<std::string, const FileRef*> cabs;
        for (const FileRef& c : tool.cabs) cabs[Lower(c.name)] = &c;
        std::vector<const FileRef*> needed;
        for (const FileRef& f : tool.files) {
            std::string bytes;
            ReadAll(downloads / f.name, bytes);
            for (const std::string& c : MsiCabs(std::vector<uint8_t>(bytes.begin(), bytes.end()))) {
                auto it = cabs.find(Lower(c));
                if (it != cabs.end() && std::find(needed.begin(), needed.end(), it->second) == needed.end())
                    needed.push_back(it->second);
            }
        }
        total = before;
        for (const FileRef* c : needed) total += c->size;
        for (const FileRef* c : needed)
            if (!fetch(*c)) return false;
        size_t i = 0;
        for (const FileRef& f : tool.files) {
            Say("Unpacking " + tool.title, f.name, double(i++) / double(tool.files.size()));
            Log("extract " + f.name);
            if (!platform_.ExtractMsi(downloads / f.name, dir, error)) return false;
            if (cancel) {
                error = "cancelled";
                return false;
            }
        }
    } else {
        for (const FileRef& f : tool.files) {
            Log("unpack " + f.name);
            bool ok = tool.kind == "tar.xz" ? UntarXz(downloads / f.name, dir, select, progress, &cancel, error)
                                            : Unzip(downloads / f.name, dir, select, progress, &cancel, error);
            if (!ok) return false;
        }
    }
    std::ofstream(stamp) << tool.stamp;
    fs::remove_all(downloads, ec);  // our own download folder
    Log(tool.title + " ready in " + U8(dir));
    return true;
}

int Builder::Git(const std::vector<std::string>& args, std::string* out, std::string& error) {
    std::vector<std::string> cmd = {U8((ToolPath("git") / "cmd" / ("git" + platform_.ExeSuffix())))};
    std::error_code ec;
    if (!fs::exists(cmd[0], ec)) cmd[0] = "git";  // the Linux test tool
    cmd.insert(cmd.end(), args.begin(), args.end());
    int code = platform_.Run(cmd, root_, BuildEnv(plan_), [&](const std::string& line) {
            if (out) *out += line + "\n";
            Log("git: " + line);
            // "Receiving objects:  45% (...)"
            size_t pct = line.find('%');
            if (pct != std::string::npos && pct >= 2) {
                size_t at = line.find_last_of(' ', pct);
                double v = std::atof(line.c_str() + (at == std::string::npos ? 0 : at + 1));
                Say("Downloading the game's source", line.substr(0, line.find(':')), v / 100.0);
            }
        }, &cancel);
    if (code != 0) error = "git " + (args.empty() ? std::string() : args[0]) + " failed (" + std::to_string(code) + "), see launcher.log";
    return code;
}

bool Builder::GetSource(std::string& error) {
    Say("Downloading the game's source");
    std::error_code ec;
    fs::path src = root_ / "source";
    if (fs::exists(src / ".git", ec)) {
        if (Git({"-C", U8(src), "fetch", "--progress", "origin", game_.branch}, nullptr, error) != 0) return false;
        // the launcher's own checkout: whatever it holds becomes the branch's head
        if (Git({"-C", U8(src), "reset", "--hard", "origin/" + game_.branch}, nullptr, error) != 0) return false;
    } else {
        fs::remove_all(src, ec);  // a clone that did not finish
        if (Git({"clone", "--progress", "--branch", game_.branch, game_.repo, U8(src)}, nullptr, error) != 0) return false;
    }
    return Git({"-C", U8(src), "submodule", "update", "--init", "--recursive", "--progress"}, nullptr, error) == 0;
}

bool Builder::PutXbe(std::string& error) {
    fs::path saved = SavedGame();
    if (saved.empty()) {
        error = "Choose the game first.";
        return false;
    }
    GameCheck copy = CheckGame(game_, saved);
    if (!copy.ok) {
        error = copy.error;
        return false;
    }
    std::vector<uint8_t> xbe;
    if (!ReadXbe(game_, copy, xbe, error)) return false;
    fs::path to = root_ / "source" / "assets" / game_.xbe;
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    std::string have;
    if (ReadAll(to, have) && have.size() == xbe.size() && std::equal(xbe.begin(), xbe.end(), have.begin())) return true;
    std::ofstream(to, std::ios::binary).write(reinterpret_cast<const char*>(xbe.data()), std::streamsize(xbe.size()));
    Log("xbe written to " + U8(to));
    return true;
}

std::map<std::string, std::string> Builder::BuildEnv(const Plan& plan) const {
    std::map<std::string, std::string> env;
    char sep = platform_.PathSeparator();
    auto join = [&](const std::vector<fs::path>& parts) {
        std::string s;
        for (const fs::path& p : parts) s += (s.empty() ? "" : std::string(1, sep)) + U8(p);
        return s;
    };
    fs::path msvc = FirstDir(ToolPath("msvc") / "VC" / "Tools" / "MSVC");
    fs::path kits = ToolPath("winsdk") / "Windows Kits" / "10";
    std::string ver = plan.sdk_version;
    std::error_code ec;
    if (ver.empty() || !fs::exists(kits / "Include" / ver, ec)) ver = U8(FirstDir(kits / "Include").filename());
    std::vector<fs::path> path = {ToolPath("llvm") / "bin", ToolPath("cmake") / "bin", ToolPath("ninja"),
                                  ToolPath("git") / "cmd", kits / "bin" / ver / "x64"};
    std::string inherited = platform_.GetEnv("PATH");
    env["PATH"] = join(path) + (inherited.empty() ? "" : std::string(1, sep) + inherited);
    if (!msvc.empty()) {
        fs::path inc = kits / "Include" / ver, lib = kits / "Lib" / ver;
        env["INCLUDE"] = join({msvc / "include", inc / "ucrt", inc / "shared", inc / "um", inc / "winrt", inc / "cppwinrt"});
        env["LIB"] = join({msvc / "lib" / "x64", lib / "ucrt" / "x64", lib / "um" / "x64"});
        env["VCToolsInstallDir"] = U8(msvc) + "\\";
        env["WindowsSdkDir"] = U8(kits) + "\\";
        env["WindowsSDKVersion"] = ver + "\\";
    }
    return env;
}

bool Builder::Compile(std::string& error) {
    fs::path src = root_ / "source";
    std::map<std::string, std::string> env = BuildEnv(plan_);
    std::string cmake = U8((ToolPath("cmake") / "bin" / ("cmake" + platform_.ExeSuffix())));
    std::error_code ec;
    if (!fs::exists(cmake, ec)) cmake = "cmake";
    // a toolchain other than the last build's: CMake's cache (its compiler and header checks) is made again
    std::string tools;
    for (const Tool& t : plan_.tools)
        if (!t.optional) tools += t.id + " " + t.stamp + "\n";
    fs::path build = src / "out" / "build" / "release", seen = build / ".launcher-tools";
    std::string last;
    // (a build folder from before this record counts as another toolchain)
    if (fs::exists(build / "CMakeCache.txt", ec) && (!ReadAll(seen, last) || last != tools)) {
        Log("the build tools changed: CMake's cache is made again");
        fs::remove(build / "CMakeCache.txt", ec);
        fs::remove_all(build / "CMakeFiles", ec);  // CMake's own folder in our build folder
    }
    Say("Preparing the build", "the first time also downloads the libraries it uses");
    // clang links with lld (no Microsoft linker in the kit)
    int code = platform_.Run({cmake, "--preset", "release", "-DCMAKE_LINKER_TYPE=LLD"}, src, env,
                             [&](const std::string& line) { Log("cmake: " + line); }, &cancel);
    if (code != 0) {
        error = "Preparing the build failed, see launcher.log";
        return false;
    }
    fs::create_directories(build, ec);
    std::ofstream(seen) << tools;
    Say("Building " + game_.title, "the first build takes 30 to 90 minutes", 0);
    code = platform_.Run({cmake, "--build", "out/build/release"}, src, env, [&](const std::string& line) {
            Log(line);
            // ninja: "[123/4567] Building CXX object ..."
            if (line.size() > 3 && line[0] == '[') {
                size_t slash = line.find('/'), close = line.find(']');
                if (slash != std::string::npos && close != std::string::npos && slash < close) {
                    double done = std::atof(line.c_str() + 1), all = std::atof(line.c_str() + slash + 1);
                    if (all > 0) Say("Building " + game_.title, std::to_string(int(done)) + " of " + std::to_string(int(all)) + " steps", done / all);
                }
            }
        }, &cancel);
    if (code != 0) {
        error = cancel ? "cancelled" : "The build failed, see launcher.log";
        return false;
    }
    return true;
}

bool Builder::Install(std::string& error) {
    fs::path out = root_ / "source" / "out" / "build" / "release";
    std::error_code ec;
    fs::create_directories(PlayDir(), ec);
    bool any = false;
    for (const auto& e : fs::directory_iterator(out, ec)) {
        std::string ext = Lower(U8(e.path().extension()));
        if (!e.is_regular_file(ec) || (ext != ".exe" && ext != ".dll")) continue;
        std::string name = Lower(U8(e.path().stem()));
        if (ext == ".exe" && name != Lower(game_.exe)) continue;  // the build's own tools (xbe_recomp) stay
        fs::copy_file(e.path(), PlayDir() / e.path().filename(), fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "Cannot copy " + U8(e.path().filename()) + " (is the game still running?)";
            return false;
        }
        any = true;
    }
    if (!any || !Built()) {
        error = "The build made no " + game_.exe + platform_.ExeSuffix();
        return false;
    }
    Log("installed in " + U8(PlayDir()));
    return true;
}

bool Builder::PathTooLong(std::string& message) const {
    size_t length = root_.wstring().size();
    if (length <= kMaxRootLength) return false;
    message = "This folder's path is too long for Microsoft's Windows SDK installer (" + std::to_string(length) +
              " characters, at most " + std::to_string(kMaxRootLength) + "): " + U8(root_) +
              ". Move " + game_.short_name + "Launcher.exe to a short folder, such as C:\\Games\\" + game_.short_name +
              ", and run it from there.";
    return true;
}

bool Builder::BuildAll(std::string& error) {
    cancel = false;
    auto steps = [&] {
        if ((!Built() && PathTooLong(error)) || !LoadToolchain(plan_, error)) return false;
        for (const Tool& t : plan_.tools)
            if (!t.optional && !InstallTool(t, error)) return false;
        return GetSource(error) && PutXbe(error) && Compile(error) && Install(error);
    };
    if (!steps()) {
        if (error != "cancelled") Log("build stopped: " + error);
        return false;
    }
    Say("Ready", "", 1);
    return true;
}

bool Builder::UpdateAvailable(bool& available, std::string& error) {
    available = false;
    fs::path src = root_ / "source";
    std::error_code ec;
    if (!fs::exists(src / ".git", ec)) return true;
    std::string head, remote;
    if (Git({"-C", U8(src), "fetch", "origin", game_.branch}, nullptr, error) != 0) return false;
    if (Git({"-C", U8(src), "rev-parse", "HEAD"}, &head, error) != 0) return false;
    if (Git({"-C", U8(src), "rev-parse", "origin/" + game_.branch}, &remote, error) != 0) return false;
    available = head != remote;
    return true;
}

bool Builder::Launch(std::string& error) { return platform_.Start(GameExe(), PlayDir(), error); }

uint64_t Builder::BuildFilesBytes() const {
    uint64_t n = 0;
    std::error_code ec;
    fs::path out = root_ / "source" / "out";
    if (!fs::exists(out, ec)) return 0;
    for (auto it = fs::recursive_directory_iterator(out, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) n += it->file_size(ec);
    return n;
}

void Builder::Remember(const std::string& key, const std::string& value) {
    std::string text;
    json j = ReadAll(root_ / "launcher.json", text) ? json::parse(text, nullptr, false) : json::object();
    if (j.is_discarded() || !j.is_object()) j = json::object();
    j[key] = value;
    std::ofstream(root_ / "launcher.json") << j.dump(2);
}

fs::path Builder::SavedMusic() const {
    std::string text;
    if (!ReadAll(root_ / "launcher.json", text)) return {};
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) return {};
    return PathOf(j.value("music", ""));
}

fs::path Builder::MusicDir() const { return PlayDir() / "hdd" / "E" / "TDATA" / "FFFE0000" / "MUSIC"; }

bool Builder::ImportMusic(const fs::path& folder, std::string& summary, std::string& error) {
    cancel = false;
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        error = "The music folder is not there: " + U8(folder);
        return false;
    }
    // ffmpeg and ffprobe: the portable ones in tools\ffmpeg (toolchain.json, optional), else on the PATH (Linux tests)
    std::string ffmpeg = "ffmpeg", ffprobe = "ffprobe";
    if (!platform_.ExeSuffix().empty()) {
        if (!LoadToolchain(plan_, error)) return false;
        auto it = std::find_if(plan_.tools.begin(), plan_.tools.end(), [](const Tool& t) { return t.id == "ffmpeg"; });
        if (it == plan_.tools.end()) {
            error = "toolchain.json lists no ffmpeg, which converts the music";
            return false;
        }
        if (!InstallTool(*it, error)) return false;
        ffmpeg = U8(ToolPath("ffmpeg") / "bin" / ("ffmpeg" + platform_.ExeSuffix()));
        ffprobe = U8(ToolPath("ffmpeg") / "bin" / ("ffprobe" + platform_.ExeSuffix()));
    }
    fs::path music = MusicDir(), staging = music / ".staging", manifest_path = music / "launcher-music.json";
    // soundtracks ripped on an Xbox and copied here are not ours to replace
    if (fs::exists(music / "ST.DB", ec) && !fs::exists(manifest_path, ec)) {
        error = "The game's music folder holds soundtracks not made here (copied from an Xbox?): " + U8(music) +
                ". Move them away to use a folder of your own music.";
        return false;
    }
    std::vector<std::string> notes;
    std::vector<MusicSoundtrack> soundtracks = ScanMusic(folder, notes);
    for (const std::string& n : notes) Log("music: " + n);
    size_t total = 0;
    for (const auto& st : soundtracks) total += st.songs.size();
    if (total == 0) {
        error = "No music found in " + U8(folder) + " (MP3, FLAC, OGG, Opus, M4A, AAC, WAV, WMA or AIFF files)";
        return false;
    }
    // what is already converted: each source by its size and time, the file it became
    std::string text;
    json last = ReadAll(manifest_path, text) ? json::parse(text, nullptr, false) : json::object();
    if (last.is_discarded() || !last.is_object()) last = json::object();
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    json now = json::object();
    size_t done = 0, converted = 0, kept = 0, failed = 0;
    std::vector<std::pair<fs::path, std::string>> moves;  // staging file, its place under MUSIC
    std::vector<std::pair<fs::path, fs::path>> taken;     // a kept file moved to staging, where it was
    auto stop = [&] {  // cancelled: the kept files go back, so the soundtracks stay as they were
        for (const auto& [from, to] : taken) fs::rename(from, to, ec);
        fs::remove_all(staging, ec);
        error = "cancelled";
        return false;
    };
    for (uint32_t i = 0; i < soundtracks.size(); i++) {
        MusicSoundtrack& st = soundtracks[i];
        std::vector<MusicSong> ok;
        for (MusicSong& song : st.songs) {
            if (cancel) return stop();
            Say("Converting your music", std::to_string(done + 1) + " of " + std::to_string(total) + ": " +
                U8(song.source.filename()), double(done) / double(total));
            done++;
            std::string key = U8(song.source);
            uint64_t size = fs::file_size(song.source, ec);
            int64_t time = int64_t(fs::last_write_time(song.source, ec).time_since_epoch().count());
            std::string staged = std::to_string(moves.size()) + ".WMA";
            const json* had = last.contains(key) ? &last[key] : nullptr;
            bool reuse = had && had->value("size", uint64_t(0)) == size && had->value("time", int64_t(0)) == time &&
                         fs::exists(music / PathOf(had->value("file", "")), ec);
            if (reuse) {
                song.name = had->value("name", song.name);
                song.ms = had->value("ms", 0u);
                fs::path was = music / PathOf(had->value("file", ""));
                fs::rename(was, staging / staged, ec);
                if (ec) reuse = false, ec.clear();
                else taken.push_back({staging / staged, was});
            }
            if (!reuse) {
                // its title, artist and length
                std::string title, artist;
                double seconds = 0;
                platform_.Run({ffprobe, "-v", "error", "-show_entries", "format=duration:format_tags=title,artist", "-of",
                               "default=noprint_wrappers=1", U8(song.source)},
                              folder, {}, [&](const std::string& line) {
                                  auto value = [&](const char* k) { return line.substr(std::strlen(k)); };
                                  if (line.rfind("duration=", 0) == 0) seconds = std::atof(value("duration=").c_str());
                                  else if (Lower(line).rfind("tag:title=", 0) == 0) title = value("TAG:title=");
                                  else if (Lower(line).rfind("tag:artist=", 0) == 0) artist = value("TAG:artist=");
                              }, &cancel);
                if (!title.empty()) song.name = artist.empty() ? title : artist + " / " + title;
                song.ms = uint32_t(seconds * 1000);
                // WMA 2, stereo 44.1 kHz: what the Xbox's decoder plays (games decode it themselves)
                std::string out_line;
                int code = platform_.Run({ffmpeg, "-nostdin", "-y", "-v", "error", "-i", U8(song.source), "-vn",
                                          "-map_metadata", "-1", "-ac", "2", "-ar", "44100", "-c:a", "wmav2", "-b:a",
                                          "160k", U8(staging / staged)},
                                         folder, {}, [&](const std::string& line) { out_line = line; }, &cancel);
                if (code != 0 || !fs::exists(staging / staged, ec)) {
                    if (cancel) return stop();
                    Log("music: " + key + " could not be converted" + (out_line.empty() ? "" : ": " + out_line));
                    failed++;
                    continue;
                }
                converted++;
            } else {
                kept++;
            }
            uint32_t index = uint32_t(ok.size());
            std::string file = SongFile(i + 1, index);
            moves.push_back({staging / staged, file});
            now[key] = {{"size", size}, {"time", time}, {"file", file}, {"name", song.name}, {"ms", song.ms}};
            ok.push_back(song);
        }
        st.songs = std::move(ok);
    }
    soundtracks.erase(std::remove_if(soundtracks.begin(), soundtracks.end(), [](const MusicSoundtrack& st) {
        return st.songs.empty();
    }), soundtracks.end());
    if (soundtracks.size() != 0) {
        // song files are numbered by their soundtrack's place: a soundtrack emptied by failures moves the ones after it
        uint32_t n = 0, sti = 0;
        for (auto& st : soundtracks) {
            sti++;
            for (uint32_t k = 0; k < st.songs.size(); k++, n++) {
                moves[n].second = SongFile(sti, k);
                now[U8(st.songs[k].source)]["file"] = moves[n].second;
            }
        }
    }
    // the old soundtrack folders go, the new files take their places
    for (const auto& e : fs::directory_iterator(music, ec))
        if (e.is_directory(ec) && e.path() != staging) fs::remove_all(e.path(), ec);
    for (const auto& [from, to] : moves) {
        fs::path dest = music / PathOf(to);
        fs::create_directories(dest.parent_path(), ec);
        fs::rename(from, dest, ec);
    }
    fs::remove_all(staging, ec);
    std::string db = SoundtrackDatabase(soundtracks);
    std::ofstream(music / "ST.DB", std::ios::binary).write(db.data(), std::streamsize(db.size()));
    std::ofstream(manifest_path) << now.dump(1);
    Remember("music", U8(folder));
    size_t songs = moves.size();
    summary = std::to_string(songs) + " songs in " + std::to_string(soundtracks.size()) + " soundtracks";
    if (failed) summary += " (" + std::to_string(failed) + " could not be converted: see launcher.log)";
    Log("music: " + summary + ", " + std::to_string(converted) + " converted, " + std::to_string(kept) +
        " kept from last time, from " + U8(folder));
    Say("Ready", "", 1);
    return true;
}

bool Builder::RemoveBuildFiles(std::string& error) {
    std::error_code ec;
    fs::remove_all(root_ / "source" / "out", ec);  // the build's own output folder
    if (ec) error = "Could not remove the build files: " + ec.message();
    return !ec;
}

}  // namespace launcher
