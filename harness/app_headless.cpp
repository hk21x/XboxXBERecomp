// the host side without a window: settings at their defaults, the run folder as the exe folder
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

#include "app/app.h"
#include "core/log.h"
#include "harness.h"

namespace app {

Settings& Get() {
    static Settings s;
    return s;
}
void Load() {}
void Save() {}
void ApplyDisplay() {}
// OUTPUT=WxH: the display the game is told about (1280x720 by default); narrower than 3:2 reports a 4:3 TV
Mode Desktop() {
    static const Mode m = [] {
        unsigned w = 0, h = 0;
        const char* e = std::getenv("OUTPUT");
        if (e && std::sscanf(e, "%ux%u", &w, &h) == 2 && w && h) return Mode{w, h};
        return Mode{1280, 720};
    }();
    return m;
}
Mode Output() { return Desktop(); }
float RenderScale(uint32_t) { return 1.0f; }
bool Widescreen() { return Desktop().width * 2 >= Desktop().height * 3; }  // as on Windows: 3:2 or wider

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

// no settings window without a window: never open, opening it does nothing
bool OverlayOpen() { return false; }
void ToggleOverlay() { LOGI("settings window: none headless"); }

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

// FPS=N presents at most N frames a second, 0 unpaced; without it the settings' fps_cap as on Windows (a game's
// init may set it), 60 when that is 0. Within that, the title's present interval as on Windows. Titles that time
// their menus by the milliseconds between frames stand still when thousands of frames take no time at all.
void Pace(uint32_t vblanks) {
    using clock = std::chrono::steady_clock;
    static const long cap = [] {
        const char* e = std::getenv("FPS");
        if (e && *e) return std::atol(e);
        return Get().fps_cap ? long(Get().fps_cap) : 60L;
    }();
    static clock::time_point next = clock::now();
    harness::g_vblanks += vblanks ? vblanks : 1;
    if (cap <= 0) return;
    long fps = PaceRate(uint32_t(cap), vblanks);
    if (fps <= 0) return;
    auto period = std::chrono::nanoseconds(1000000000LL / fps);
    auto now = clock::now();
    next += period;
    if (next < now - period) next = now;
    std::this_thread::sleep_until(next);
}

}  // namespace app
