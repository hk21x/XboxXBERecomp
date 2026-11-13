// The host side on macOS (and any platform without Win32): an SDL3 window, its event loop on the main thread, the keys
// by Windows virtual key code as the settings keep them, frame pacing, and stand ins for what only Windows has (the
// splash, the handover between images, the game code DLL, screenshots).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <thread>
#include <unistd.h>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "core/vk.h"
#include "gpu/backend.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

namespace app {

namespace {

SDL_Window* g_window = nullptr;
SDL_MetalView g_metal_view = nullptr;
std::atomic<bool> g_active{true};
std::atomic<bool> g_keys[256];
Uint32 g_apply_event = 0;  // ApplyDisplay from another thread: the main thread applies it

// SDL scancodes to the Windows virtual keys the bindings name
uint16_t VirtualKey(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return uint16_t('A' + (sc - SDL_SCANCODE_A));
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return uint16_t('1' + (sc - SDL_SCANCODE_1));
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return uint16_t(VK_F1 + (sc - SDL_SCANCODE_F1));
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return uint16_t(VK_NUMPAD1 + (sc - SDL_SCANCODE_KP_1));
    switch (sc) {
        case SDL_SCANCODE_0: return '0';
        case SDL_SCANCODE_KP_0: return VK_NUMPAD0;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return VK_RETURN;
        case SDL_SCANCODE_ESCAPE: return VK_ESCAPE;
        case SDL_SCANCODE_BACKSPACE: return VK_BACK;
        case SDL_SCANCODE_TAB: return VK_TAB;
        case SDL_SCANCODE_SPACE: return VK_SPACE;
        case SDL_SCANCODE_LEFT: return VK_LEFT;
        case SDL_SCANCODE_RIGHT: return VK_RIGHT;
        case SDL_SCANCODE_UP: return VK_UP;
        case SDL_SCANCODE_DOWN: return VK_DOWN;
        case SDL_SCANCODE_INSERT: return VK_INSERT;
        case SDL_SCANCODE_DELETE: return VK_DELETE;
        case SDL_SCANCODE_HOME: return VK_HOME;
        case SDL_SCANCODE_END: return VK_END;
        case SDL_SCANCODE_PAGEUP: return VK_PRIOR;
        case SDL_SCANCODE_PAGEDOWN: return VK_NEXT;
        case SDL_SCANCODE_LSHIFT: return VK_LSHIFT;
        case SDL_SCANCODE_RSHIFT: return VK_RSHIFT;
        case SDL_SCANCODE_LCTRL: return VK_LCONTROL;
        case SDL_SCANCODE_RCTRL: return VK_RCONTROL;
        case SDL_SCANCODE_LALT: return VK_LMENU;
        case SDL_SCANCODE_RALT: return VK_RMENU;
        case SDL_SCANCODE_COMMA: return VK_OEM_COMMA;
        case SDL_SCANCODE_PERIOD: return VK_OEM_PERIOD;
        case SDL_SCANCODE_SLASH: return VK_OEM_2;
        case SDL_SCANCODE_SEMICOLON: return VK_OEM_1;
        case SDL_SCANCODE_APOSTROPHE: return VK_OEM_7;
        case SDL_SCANCODE_LEFTBRACKET: return VK_OEM_4;
        case SDL_SCANCODE_RIGHTBRACKET: return VK_OEM_6;
        case SDL_SCANCODE_MINUS: return VK_OEM_MINUS;
        case SDL_SCANCODE_EQUALS: return VK_OEM_PLUS;
        case SDL_SCANCODE_GRAVE: return VK_OEM_3;
        case SDL_SCANCODE_BACKSLASH: return VK_OEM_5;
        default: return 0;
    }
}

void SetKey(uint16_t vk, bool down) {
    if (vk < 256) g_keys[vk] = down;
    // the generic modifiers as Windows reports them too
    if (vk == VK_LSHIFT || vk == VK_RSHIFT) g_keys[VK_SHIFT] = g_keys[VK_LSHIFT] || g_keys[VK_RSHIFT];
    if (vk == VK_LCONTROL || vk == VK_RCONTROL) g_keys[VK_CONTROL] = g_keys[VK_LCONTROL] || g_keys[VK_RCONTROL];
    if (vk == VK_LMENU || vk == VK_RMENU) g_keys[VK_MENU] = g_keys[VK_LMENU] || g_keys[VK_RMENU];
}

SDL_DisplayID Display() {
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    SDL_DisplayID id = SDL_GetPrimaryDisplay();
    uint32_t n = Get().monitor;
    if (ids && n && int(n) <= count) id = ids[n - 1];
    SDL_free(ids);
    return id;
}

void ApplyNow() {
    if (!g_window) return;
    const Settings& s = Get();
    if (s.display_mode == DisplayMode::Windowed) {
        SDL_SetWindowFullscreen(g_window, false);
        Mode out = Output();
        // the output in points: SDL sizes a high density window in points, the drawable is its pixels
        float scale = SDL_GetWindowDisplayScale(g_window);
        if (scale <= 0) scale = 1;
        SDL_SetWindowSize(g_window, int(out.width / scale), int(out.height / scale));
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED_DISPLAY(Display()), SDL_WINDOWPOS_CENTERED_DISPLAY(Display()));
    } else {
        // borderless and fullscreen both take the whole display as a desktop fullscreen window: macOS has no
        // exclusive mode worth a mode switch
        SDL_SetWindowFullscreenMode(g_window, nullptr);
        SDL_SetWindowFullscreen(g_window, true);
    }
}

void OnCrash(int sig, siginfo_t* info, void*) {
    char line[128];
    int n = std::snprintf(line, sizeof line, "crash: signal %d at %p\n", sig, info->si_addr);
    if (n > 0) (void)!write(2, line, size_t(n));
    LOGE("crash: signal %d at %p", sig, info->si_addr);
    void* frames[48];
    backtrace_symbols_fd(frames, backtrace(frames, 48), 2);
    logx::Flush();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

}  // namespace

bool KeyHeld(uint16_t vk) { return vk < 256 && g_keys[vk]; }
bool WindowInFront() { return g_active; }

std::filesystem::path ExeDir() {
    static const std::filesystem::path dir = [] {
        char path[4096];
        uint32_t size = sizeof(path);
        if (_NSGetExecutablePath(path, &size) != 0) return std::filesystem::current_path();
        std::error_code ec;
        auto p = std::filesystem::canonical(path, ec);
        auto d = (ec ? std::filesystem::path(path) : p).parent_path();
        // inside an app bundle: the folder holding the bundle, where assets and the settings live
        if (d.filename() == "MacOS" && d.parent_path().filename() == "Contents") d = d.parent_path().parent_path().parent_path();
        return d;
    }();
    return dir;
}

int Instance() { return 0; }
void* MetalLayer() { return g_metal_view ? SDL_Metal_GetLayer(g_metal_view) : nullptr; }
void* Window() { return g_window; }

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void ApplyDisplay() {
    SDL_Event e{};
    e.type = g_apply_event;
    SDL_PushEvent(&e);
}

void Quit() {
    LOGR("quit asked for");
    SDL_Event e{};
    e.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&e);
}

bool Paused() { return Get().pause_unfocused && !g_active; }

void Pace(uint32_t vblanks) {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    if (Paused()) {
        LOGR("paused: the window is not in front");
        while (Paused()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        LOGR("resumed");
        next = clock::now();
    }
    uint32_t cap = PaceRate(Get().fps_cap, vblanks);
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    static clock::time_point last = now, report = now + std::chrono::seconds(10);
    static uint32_t frames = 0, slow = 0;
    static double longest = 0;
    double took = std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    frames++;
    if (took > 1.5 * std::chrono::duration<double, std::milli>(period).count()) slow++, longest = std::max(longest, took);
    if (now >= report) {
        uint32_t compiled = gpu::g_shaders_compiled.exchange(0);
        if (slow) LOGR("frames: %u of %u in the last 10 s over %.1f ms (%u a second paced), the longest %.1f ms; "
                       "%u shaders compiled", slow, frames, 1.5 * std::chrono::duration<double, std::milli>(period).count(),
                       cap, longest, compiled);
        report = now + std::chrono::seconds(10);
        frames = slow = 0;
        longest = 0;
    }
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
}

// the display, by the monitor setting
void* Monitor() { return reinterpret_cast<void*>(uintptr_t(Display())); }

std::vector<std::string> Monitors() {
    std::vector<std::string> names;
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    for (int i = 0; ids && i < count; i++) {
        const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(ids[i]);
        const char* name = SDL_GetDisplayName(ids[i]);
        int w = m ? int(m->w * m->pixel_density) : 0, h = m ? int(m->h * m->pixel_density) : 0;
        names.push_back(std::to_string(i + 1) + ": " + (name ? name : "display") + " (" + std::to_string(w) + " x " +
                        std::to_string(h) + (ids[i] == SDL_GetPrimaryDisplay() ? ", primary)" : ")"));
    }
    SDL_free(ids);
    return names;
}

// in pixels: a Retina display's own resolution, not its points
Mode Desktop() {
    if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(Display()))
        return {uint32_t(m->w * m->pixel_density), uint32_t(m->h * m->pixel_density)};
    return {1920, 1080};
}

std::vector<Mode> HostModes() {
    std::vector<Mode> modes;
    int count = 0;
    SDL_DisplayMode** list = SDL_GetFullscreenDisplayModes(Display(), &count);
    for (int i = 0; list && i < count; i++) {
        Mode m{uint32_t(list[i]->w * list[i]->pixel_density), uint32_t(list[i]->h * list[i]->pixel_density)};
        if (m.height < 480) continue;
        if (std::none_of(modes.begin(), modes.end(), [&](const Mode& o) { return o.width == m.width && o.height == m.height; }))
            modes.push_back(m);
    }
    SDL_free(list);
    if (modes.empty()) modes.push_back(Desktop());
    std::sort(modes.begin(), modes.end(), [](const Mode& a, const Mode& b) {
        return a.width != b.width ? a.width > b.width : a.height > b.height;
    });
    return modes;
}

// what only the Windows build has
bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}
bool PrepareGameCode(const GameFiles&) { return true; }  // the code is linked into the executable
int PrepareGameCodeHeadless(const GameFiles&) { return 0; }

namespace {
std::atomic<bool> g_shot{false};
}
void RequestScreenshot() { g_shot = true; }
bool TakeScreenshotRequest() { return g_shot.exchange(false); }
// B8G8R8A8 rows as a TGA under screenshots/ beside the game
std::atomic<int64_t> g_shot_saved{0};
void SaveScreenshot(std::vector<uint8_t> bgra, uint32_t w, uint32_t h) {
    std::error_code ec;
    auto dir = ExeDir() / "screenshots";
    std::filesystem::create_directories(dir, ec);
    char name[64];
    std::time_t now = std::time(nullptr);
    std::strftime(name, sizeof(name), "pgr1-%Y%m%d-%H%M%S", std::localtime(&now));
    static int n = 0;
    auto path = dir / (std::string(name) + "-" + std::to_string(n++) + ".tga");
    FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f) return;
    uint8_t hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, uint8_t(w), uint8_t(w >> 8), uint8_t(h), uint8_t(h >> 8), 32, 0x28};
    std::fwrite(hdr, 1, sizeof(hdr), f);
    for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;  // the frame's alpha is the game's, not coverage
    std::fwrite(bgra.data(), 1, bgra.size(), f);
    std::fclose(f);
    g_shot_saved = SDL_GetTicks();
    LOGI("screenshot: %s", path.string().c_str());
}
bool ScreenshotJustSaved() { return g_shot_saved && SDL_GetTicks() - uint64_t(g_shot_saved) < 3000; }

}  // namespace app

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    struct sigaction sa{};
    sa.sa_sigaction = app::OnCrash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);

    logx::Init((app::ExeDir() / "logs").string(), app::kGame.log, false);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        LOGE("SDL failed to start: %s", SDL_GetError());
        return 1;
    }
    app::Load();
    if (app::kGame.init) app::kGame.init();
    app::LogSystem();

    app::GameFiles game;
    if (!app::FindGame(game)) return 1;

    std::string title;
    for (const wchar_t* t = app::kGame.title; t && *t; t++) title += *t < 128 ? char(*t) : '?';
    app::g_apply_event = SDL_RegisterEvents(1);
    app::g_window = SDL_CreateWindow(title.c_str(), 1280, 720, SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!app::g_window) {
        LOGE("no window: %s", SDL_GetError());
        return 1;
    }
    // the renderer's layer, made here: AppKit views belong to the main thread
    app::g_metal_view = SDL_Metal_CreateView(app::g_window);
    app::ApplyNow();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    // the cursor is AppKit's, which the render thread (where ImGui's frames are made) may not change
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad |
                      ImGuiConfigFlags_NoMouseCursorChange;
    app::OverlayInit();
    ImGui_ImplSDL3_InitForMetal(app::g_window);

    input::Init();
    if (app::kGame.window) app::kGame.window(app::g_window);
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / "hdd").string())) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), "The game failed to start, see the log in the logs folder.",
                                 app::g_window);
        return 1;
    }

    for (bool running = true; running;) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 2)) {
            SDL_UpdateGamepads();
            continue;
        }
        do {
            if (app::OverlayOpen()) {
                std::lock_guard lock(app::UiLock());
                ImGui_ImplSDL3_ProcessEvent(&e);
            }
            if (e.type == app::g_apply_event) {
                app::ApplyNow();
                continue;
            }
            switch (e.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    running = false;
                    break;
                case SDL_EVENT_WINDOW_FOCUS_GAINED: app::g_active = true; break;
                case SDL_EVENT_WINDOW_FOCUS_LOST:
                    app::g_active = false;
                    for (auto& k : app::g_keys) k = false;
                    break;
                case SDL_EVENT_KEY_DOWN:
                case SDL_EVENT_KEY_UP: {
                    bool down = e.type == SDL_EVENT_KEY_DOWN;
                    uint16_t vk = app::VirtualKey(e.key.scancode);
                    // the overlay waits for a key to bind: it takes the press
                    if (down && !e.key.repeat && vk && app::OverlayOpen()) {
                        std::lock_guard lock(app::UiLock());
                        if (app::OverlayKey(vk)) break;
                    }
                    if (vk) app::SetKey(vk, down);
                    if (down && !e.key.repeat) {
                        if (vk == VK_F1) {
                            std::lock_guard lock(app::UiLock());
                            app::ToggleOverlay();
                        } else if (vk == VK_F12) {
                            app::RequestScreenshot();
                        } else if (vk == VK_RETURN && (e.key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))) {
                            // Alt/Cmd+Enter: windowed and back
                            auto& s = app::Get();
                            s.display_mode = s.display_mode == app::DisplayMode::Windowed ? app::DisplayMode::Borderless
                                                                                         : app::DisplayMode::Windowed;
                            app::Save();
                            app::ApplyNow();
                        }
                    }
                    break;
                }
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                case SDL_EVENT_MOUSE_BUTTON_UP: {
                    bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                    uint16_t vk = e.button.button == SDL_BUTTON_LEFT     ? VK_LBUTTON
                                  : e.button.button == SDL_BUTTON_RIGHT  ? VK_RBUTTON
                                  : e.button.button == SDL_BUTTON_MIDDLE ? VK_MBUTTON
                                  : e.button.button == SDL_BUTTON_X1     ? VK_XBUTTON1
                                                                         : VK_XBUTTON2;
                    app::SetKey(vk, down);
                    break;
                }
                case SDL_EVENT_MOUSE_MOTION:
                    if (input::MouseLooking()) input::MouseMoved(int(e.motion.xrel), int(e.motion.yrel));
                    break;
                case SDL_EVENT_MOUSE_WHEEL: input::MouseWheel(int(e.wheel.y * 120)); break;
                case SDL_EVENT_GAMEPAD_ADDED: input::DeviceEvent(true, e.gdevice.which); break;
                case SDL_EVENT_GAMEPAD_REMOVED: input::DeviceEvent(false, e.gdevice.which); break;
                default: break;
            }
        } while (running && SDL_PollEvent(&e));
    }
    LOGR("the window closed");
    logx::Flush();
    kernel::Shutdown(0);
    return 0;
}
