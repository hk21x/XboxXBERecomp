#pragma once
// The launcher's screen (screen.cpp): classic Steam's olive, as the game's settings window, with a section list on the
// left and what is going on in the footer. The window, its device and the shell's dialogs are main.cpp's.
#include <imgui.h>

#include <atomic>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/builder.h"
#include "core/update.h"
#include "core/util.h"

namespace launcher {

namespace fs = std::filesystem;

// The window's state; the worker thread writes status and log under the lock
struct App {
    GameConfig game;
    std::unique_ptr<Http> http = MakeHttp();
    std::unique_ptr<Platform> platform = MakePlatform();
    std::unique_ptr<Builder> builder;
    fs::path root;

    GameCheck copy;
    bool agreed = false;
    std::atomic<bool> busy{false}, update{false}, checked_update{false};
    // a newer release's launcher (core/update.h): offered at the top until updated or put off
    std::mutex release_lock;
    Release release;
    std::atomic<bool> launcher_update{false}, later{false}, restart{false};
    std::thread worker;
    std::mutex lock;
    Status status;
    std::string error, done_message;
    std::deque<std::string> log;
    uint64_t build_bytes = 0;
    int page = 0;              // the section shown: ScreenPage
    fs::path music;            // the music folder imported last
    bool music_job = false;    // the job running or last run imports the music: its result shows under Your music
    std::string music_summary;

    void Start(std::function<bool(std::string&)> job, std::string done) {
        if (busy) return;
        if (worker.joinable()) worker.join();
        busy = true;
        {
            std::lock_guard l(lock);
            error.clear();
            done_message.clear();
            status = {};
        }
        worker = std::thread([this, job, done] {
            std::string e;
            bool ok = job(e);
            std::lock_guard l(lock);
            if (!ok) error = e == "cancelled" ? "Stopped. Press the button again to carry on where it left off." : e;
            else done_message = done;
            status = {};
            build_bytes = builder->BuildFilesBytes();
            busy = false;
        });
    }
};

// main.cpp's (tools' stand-ins in the preview)
fs::path Pick(void* owner_window, bool folder, const wchar_t* title = nullptr);
void OpenUrl(const std::string& url);
void OpenFolder(const fs::path& dir);
uint64_t FreeBytes(const fs::path& dir);
bool UpdateLauncher(App& app, const Release& r, std::string& error);

void ScreenStyle(float scale);
ImFont* ScreenFonts(const fs::path& font_folder, float scale);  // the heading's font; the body's is the default
void DrawScreen(App& app, void* window, ImFont* heading);

}  // namespace launcher
