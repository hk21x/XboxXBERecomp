#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/achievements.h"

namespace input {
struct Pad;
}

// the host side: window, settings
namespace app {

enum class DisplayMode : uint32_t { Fullscreen, Borderless, Windowed };

struct Settings {
    DisplayMode display_mode = DisplayMode::Borderless;
    uint32_t width = 0, height = 0;  // 0 is the desktop mode
    uint32_t render_percent = 100;   // of the output height
    uint32_t fps_cap = 60;           // 0: only the title's present interval
    bool vsync = true;
    uint32_t aniso = 0;              // 0 as the game asks
    std::string join_ip;                   // co-op: a host past the LAN the system link search also asks
    // 0 from the output's shape, 1 4:3, 2 16:9 (the console's TV setting, at boot), 3 widescreen filling the screen's
    // shape (ultrawide: the title's 3D views widened by its GameInfo code, the frame shown at the screen's shape)
    uint32_t aspect = 0;
    // controls, by input::Control: the physical pad input that drives each console input, and up to two keys (Windows
    // virtual key codes, 0 none) that press it on controller 1
    std::vector<uint8_t> pad_map;    // empty: each input drives itself
    std::vector<std::array<uint16_t, 2>> keys;  // empty: the default keys
    bool keyboard = true;
    uint32_t monitor = 0;            // 0 the primary display, n the nth display Windows lists
    bool pause_unfocused = false;    // the game stands still, silent, while its window is not in front
    bool show_fps = false;           // frames a second in a corner
    uint32_t antialiasing = 0;       // 0 none, 1 FXAA on the finished frame, 2 3 4 MSAA 2x 4x 8x on the scene
    uint32_t renderer = 0;           // 0 Direct3D 12, 1 Direct3D 11, at the next start
};
// the default keys for input::Control c
std::array<uint16_t, 2> DefaultKeys(int c);
std::string KeyToName(uint16_t vk);  // settings file names of Windows virtual keys
uint16_t KeyFromName(const std::string& name);
// the in-game settings window (F1): open, and drawn each frame inside the ImGui frame (overlay.cpp)
bool OverlayOpen();
void ToggleOverlay();
void DrawOverlay();
// a key pressed while the overlay waits for one to bind (window thread); true takes it
bool OverlayKey(uint16_t vk);

Settings& Get();
void Load();
void Save();
void LogSettings();  // every setting, as the settings file has them
void LogSystem();    // the machine and the settings, at a run's start
void ApplyDisplay();  // any thread
struct Mode {
    uint32_t width, height;
};
Mode Desktop();
void* Monitor();                  // HMONITOR of the monitor setting (the primary display when it is gone)
std::vector<std::string> Monitors();  // the displays' names, in the monitor setting's order from 1
bool Paused();                    // pause_unfocused and the window is not in front (Windows)
void DrawHud(double fps);         // the frame counter, inside the ImGui frame (overlay.cpp)
void FeedPad();                   // controllers to the settings window's navigation, before each ImGui frame
Mode Output();
float RenderScale(uint32_t guest_height);
// the output is 3:2 or wider: the console reports a widescreen TV and the frame is shown 16:9, else 4:3 as on a 4:3 TV
bool Widescreen();
// the shape the frame is shown at: 4:3, 16:9, or the screen's when the aspect setting fills it (never under 16:9)
float FrameAspect();

std::filesystem::path ExeDir();
// the game in assets (game_files.cpp): its folder and this executable's xbe, from the extracted files or a disc
// image (.iso), which it mounts as D:; false after telling the player what is missing
struct GameFiles {
    std::filesystem::path dir;
    std::vector<uint8_t> xbe;
};
bool FindGame(GameFiles& game);
int Instance();  // --instance N on the command line, 0 without: a second copy on this PC for system link tests
void* Window();  // HWND
// boot splash: GameInfo::splash .bmp or .png beside the exe, up before the game window
bool OpenSplash();        // window thread, at launch; false without one, and the game window opens at once
void SplashFirstFrame();  // the game's first Swap: waits out the splash's minimum, then opens the game window
void CloseSplash();       // window thread
void OpenGameWindow(bool apply = true);  // any thread but the window thread's; returns once the window is up
// relaunched: the previous instance keeps its window up until this one has shown its first frames (handover.cpp)
bool StartHandover();   // window thread, at launch; true: the game window starts hidden
bool HandingOver();
void FinishHandover();  // any thread but the window thread's: opens the game window, lets the old instance go

// held around ImGui: the window thread's messages and the renderer's frame
std::recursive_mutex& UiLock();

// what the game repo defines once
struct Option {  // a setting in the settings file, flag or value
    const char* key;
    bool* flag;
    uint32_t* value;
    const char* label = nullptr;  // the settings window's name for it, else the key
    const char* help = nullptr;   // a line under it in the settings window
};
struct GameInfo {
    const wchar_t* title;        // window and splash
    const wchar_t* short_title;  // message boxes
    const wchar_t* name;         // window classes, crash dumps, handover event
    const char* log;             // log prefix
    const char* settings;        // settings file beside the exe
    const wchar_t* splash;       // .bmp or .png beside the exe
    const wchar_t* missing;      // message when neither the game files nor a disc image are found
    std::vector<std::pair<const char*, const char*>> images;  // xbe file name, executable name
    std::vector<Option> options;
    // the settings file's toml::table, for what options cannot hold
    void (*load)(const void* table);
    void (*save)(void* table);
    void (*init)();                // after the settings load, before the window and boot
    void (*loaded)();              // image in memory, before its entry runs
    void (*window)(void* hwnd);    // the game window is up, before boot
    // window thread, each message after ImGui's; true returns result instead of the host's handling
    bool (*message)(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
    void (*ui)(double fps);        // each frame inside the ImGui frame, UiLock held
    void (*pad)(int port, input::Pad& pad);  // a polled pad before the game gets it
    void (*rumble)(int port, uint16_t& left, uint16_t& right);
    std::vector<achievements::Def> achievements = {};  // core/achievements.h
    // other releases' xbe SHA-1s whose code and data are the recompiled one's, differing only in the signature and
    // certificate (region, version, keys): started without the "different release" question
    std::vector<const char*> same_code = {};
};
extern const GameInfo kGame;

// sleeps out the rest of the frame: the title's present interval (vertical blanks a frame at 60 Hz, 0 unpaced)
// within the fps cap
void Pace(uint32_t vblanks = 1);
// frames a second for a title asking for vblanks within cap (0 unlimited); 0 unpaced
inline uint32_t PaceRate(uint32_t cap, uint32_t vblanks) {
    if (!vblanks) return cap;
    uint32_t fps = 60 / vblanks;
    return cap && cap < fps ? cap : fps;
}

}  // namespace app
