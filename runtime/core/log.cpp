#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace logx {

static std::mutex g_mutex;
static FILE* g_file = nullptr;
static std::string g_path;
static auto g_start = std::chrono::steady_clock::now();

constexpr int kKeep = 30;

// the run number of prefix_NNN.log, 0 for another name
static int RunNumber(const fs::path& file, const std::string& prefix) {
    std::string name = file.filename().string();
    if (file.extension() != ".log" || name.size() <= prefix.size() + 1 || name.compare(0, prefix.size() + 1, prefix + "_"))
        return 0;
    return std::atoi(name.c_str() + prefix.size() + 1);
}

void Init(const std::string& dir, const std::string& prefix, bool append) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!kVerbose) {
        g_path = (fs::path(dir) / (prefix + ".log")).string();
        // the last runs' logs are kept: prefix.log moves to prefix.1.log, that to prefix.2.log, and so on (a run
        // started straight after another no longer loses the one before)
        if (!append && fs::exists(g_path, ec)) {
            constexpr int kKeepRelease = 5;
            auto old = [&](int i) { return (fs::path(dir) / (prefix + "." + std::to_string(i) + ".log")); };
            fs::remove(old(kKeepRelease), ec);
            for (int i = kKeepRelease - 1; i >= 1; i--) fs::rename(old(i), old(i + 1), ec);
            fs::rename(g_path, old(1), ec);
        }
        g_file = std::fopen(g_path.c_str(), append ? "a" : "w");
        return;
    }
    std::vector<std::pair<int, fs::path>> runs;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (int n = RunNumber(e.path(), prefix)) runs.emplace_back(n, e.path());
    std::sort(runs.begin(), runs.end());
    for (size_t i = 0; i + kKeep <= runs.size(); i++) fs::remove(runs[i].second, ec);
    char name[256];
    std::snprintf(name, sizeof(name), "%s_%03d.log", prefix.c_str(), runs.empty() ? 1 : runs.back().first + 1);
    g_path = (fs::path(dir) / name).string();
    g_file = std::fopen(g_path.c_str(), "w");
}

const std::string& Path() { return g_path; }

static std::string g_pending;          // lines queued for the writer (under g_mutex)
static bool g_urgent = false;          // a warning or error queued: write at once
static std::condition_variable g_cv;
static std::mutex g_io;                // the file and stderr: the writer's, and Flush's

// what is queued, taken and written in one go under g_io (the lines reach the file in order)
static void WriteOut() {
    std::lock_guard<std::mutex> lock(g_io);
    std::string out;
    {
        std::lock_guard<std::mutex> queue(g_mutex);
        out.swap(g_pending);
    }
    if (out.empty()) return;
    if (g_file) {
        std::fwrite(out.data(), 1, out.size(), g_file);
        std::fflush(g_file);
    }
    std::fwrite(out.data(), 1, out.size(), stderr);
}

// under g_mutex: the writer thread, started with the first line
static void StartWriter() {
    static bool started = false;
    if (started) return;
    started = true;
    std::thread([] {
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                g_cv.wait_for(lock, std::chrono::milliseconds(250), [] { return g_urgent; });
                g_urgent = false;
            }
            WriteOut();
        }
    }).detach();
}

void Write(Level lv, const char* fmt, ...) {
    if (!kVerbose && lv < Level::Error) return;
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    static const char* names[] = {"debug", "info", "warn", "error", "info"};
    auto tid = std::hash<std::thread::id>()(std::this_thread::get_id()) & 0xFFFF;
    char line[4300];
    int n = kVerbose ? std::snprintf(line, sizeof(line), "[%9.3f] [%s] [t%04zx] %s\n", t, names[int(lv)], size_t(tid), msg)
                     : std::snprintf(line, sizeof(line), "[%9.3f] %s%s\n", t, lv == Level::Error ? "error: " : "", msg);
    n = std::min(n, int(sizeof(line)) - 1);
    // the line is only queued: the writer thread puts it in the file (and stderr) within 250 ms, warnings and errors
    // at once. A write to the disk, or to the network when the game runs from a share, never holds up the caller (a
    // render thread waiting 400 ms on one flush was a hitch on Harry's PC)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        StartWriter();
        g_pending.append(line, size_t(n));
        if (lv == Level::Warn || lv == Level::Error) g_urgent = true;
    }
    if (lv == Level::Warn || lv == Level::Error) g_cv.notify_one();
}

// what is queued, into the file and stderr now (exit, crash, relaunch)
void Flush() { WriteOut(); }

}  // namespace logx
