// the host side without a window: settings at their defaults, the run folder as the exe folder
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

#include "app/app.h"

namespace app {

Settings& Get() {
    static Settings s;
    return s;
}
void Load() {}
void Save() {}
void ApplyDisplay() {}
Mode Desktop() { return {1280, 720}; }
Mode Output() { return Desktop(); }
float RenderScale(uint32_t) { return 1.0f; }
bool Widescreen() { return true; }

std::filesystem::path ExeDir() {
    static const std::filesystem::path dir = std::filesystem::current_path();
    return dir;
}
int Instance() {
    static const int n = [] {
        const char* e = std::getenv("INSTANCE");
        return e ? std::atoi(e) : 0;
    }();
    return n;
}
void* Window() { return nullptr; }

bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

// FPS=N presents at most N frames a second (default 60, as the console shows them; 0 runs unpaced). Titles that time
// their menus by the milliseconds between frames stand still when thousands of frames take no time at all.
void Pace() {
    using clock = std::chrono::steady_clock;
    static const long fps = [] {
        const char* e = std::getenv("FPS");
        return e && *e ? std::atol(e) : 60L;
    }();
    static clock::time_point next = clock::now();
    if (fps <= 0) return;
    auto period = std::chrono::nanoseconds(1000000000LL / fps);
    auto now = clock::now();
    next += period;
    if (next < now - period) next = now;
    std::this_thread::sleep_until(next);
}

}  // namespace app
