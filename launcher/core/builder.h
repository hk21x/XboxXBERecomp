#pragma once
// The launcher's work, step by step, under one folder (root):
//   tools\    clang, CMake, Ninja, MinGit, the C++ libraries, the Windows SDK (portable, nothing installed)
//   source\   the game's repository (git), its xbe in source\assets for the build
//   play\     the built exe, its settings, saves and logs; play\assets is the player's game (a junction to their
//             folder, or their disc image linked or copied)
//   launcher.json   the player's choices (their game's path)
// Each step can run again: what is already done is kept (tools by their stamp, the source by git).
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

#include "core/game.h"
#include "core/net.h"
#include "core/plan.h"
#include "core/platform.h"

namespace launcher {

struct Status {
    std::string step;    // "Downloading Clang 19.1.7"
    std::string detail;  // "312 of 845 MB"
    double fraction = -1;  // 0..1, -1 when unknown
};

class Builder {
public:
    Builder(const GameConfig& game, const std::filesystem::path& root, Http& http, Platform& platform);

    // where messages go: a status for the window, each log line (also written to root\launcher.log)
    std::function<void(const Status&)> on_status;
    std::function<void(const std::string&)> on_log;
    std::atomic<bool> cancel{false};
    std::string fallback_toolchain;  // toolchain.json as the launcher was built with, when neither download nor copy

    const std::filesystem::path& Root() const { return root_; }
    std::filesystem::path PlayDir() const { return root_ / "play"; }
    std::filesystem::path GameExe() const;
    bool Built() const;  // the exe is in play

    // the player's copy: checked, remembered, linked into play\assets
    bool SetGame(const GameCheck& copy, std::string& error);
    std::filesystem::path SavedGame() const;  // the copy remembered from last time, empty if none

    // the whole build: tools, source, xbe, compile, install. Returns false with the reason
    bool BuildAll(std::string& error);
    bool UpdateAvailable(bool& available, std::string& error);  // the source behind its branch's head
    bool Launch(std::string& error);
    uint64_t BuildFilesBytes() const;  // source\out, removable once built
    bool RemoveBuildFiles(std::string& error);

    // the game's Xbox custom soundtracks (GameConfig::music) from a folder of the player's music: each file converted
    // to WMA with ffmpeg (fetched the first time), ST.DB written, the folder remembered; summary: what was done
    bool ImportMusic(const std::filesystem::path& folder, std::string& summary, std::string& error);
    std::filesystem::path SavedMusic() const;  // the folder imported last, empty if none
    std::filesystem::path MusicDir() const;    // where the game finds them: play\hdd\E\TDATA\FFFE0000\MUSIC

    // steps, for the test tool
    bool LoadToolchain(Plan& plan, std::string& error);
    bool InstallTool(const Tool& tool, std::string& error);
    bool GetSource(std::string& error);
    bool PutXbe(std::string& error);
    bool Compile(std::string& error);
    bool Install(std::string& error);
    std::map<std::string, std::string> BuildEnv(const Plan& plan) const;

private:
    void Log(const std::string& line);
    void Remember(const std::string& key, const std::string& value);  // an entry of launcher.json, the others kept
    void Say(const std::string& step, const std::string& detail = {}, double fraction = -1);
    int Git(const std::vector<std::string>& args, std::string* out, std::string& error);
    std::filesystem::path ToolPath(const std::string& id) const { return root_ / "tools" / id; }

    GameConfig game_;
    std::filesystem::path root_;
    Http& http_;
    Platform& platform_;
    Plan plan_;
    std::mutex log_lock_;
};

std::string Megabytes(uint64_t bytes);

}  // namespace launcher
