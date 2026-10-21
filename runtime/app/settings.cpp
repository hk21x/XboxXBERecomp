// the settings file beside the exe
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <atomic>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};

std::filesystem::path File() { return ExeDir() / kGame.settings; }

const char* kAspects[] = {"auto", "4:3", "16:9", "screen"};
constexpr uint32_t kAspectCount = 4;
const char* kAntialiasing[] = {"off", "fxaa", "msaa2", "msaa4", "msaa8"};
constexpr uint32_t kAntialiasingCount = 5;
const char* kRenderers[] = {"d3d12", "d3d11"};
constexpr uint32_t kRendererCount = 2;

// the displays in Windows' order
std::vector<HMONITOR> MonitorList() {
    std::vector<HMONITOR> list;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
        reinterpret_cast<std::vector<HMONITOR>*>(p)->push_back(m);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&list));
    return list;
}

// keys by name in the settings file: letters, digits and these
struct KeyName {
    uint16_t vk;
    const char* name;
};
constexpr KeyName kKeyNames[] = {
    {VK_LEFT, "left"}, {VK_RIGHT, "right"}, {VK_UP, "up"}, {VK_DOWN, "down"}, {VK_RETURN, "enter"},
    {VK_SPACE, "space"}, {VK_ESCAPE, "escape"}, {VK_TAB, "tab"}, {VK_BACK, "backspace"}, {VK_SHIFT, "shift"},
    {VK_LSHIFT, "left_shift"}, {VK_RSHIFT, "right_shift"}, {VK_CONTROL, "ctrl"}, {VK_LCONTROL, "left_ctrl"},
    {VK_RCONTROL, "right_ctrl"}, {VK_MENU, "alt"}, {VK_INSERT, "insert"}, {VK_DELETE, "delete"}, {VK_HOME, "home"},
    {VK_END, "end"}, {VK_PRIOR, "page_up"}, {VK_NEXT, "page_down"}, {VK_NUMPAD0, "num0"}, {VK_NUMPAD1, "num1"},
    {VK_NUMPAD2, "num2"}, {VK_NUMPAD3, "num3"}, {VK_NUMPAD4, "num4"}, {VK_NUMPAD5, "num5"}, {VK_NUMPAD6, "num6"},
    {VK_NUMPAD7, "num7"}, {VK_NUMPAD8, "num8"}, {VK_NUMPAD9, "num9"}, {VK_OEM_COMMA, "comma"},
    {VK_OEM_PERIOD, "period"}, {VK_OEM_2, "slash"}, {VK_OEM_1, "semicolon"}, {VK_OEM_7, "quote"},
    {VK_OEM_4, "left_bracket"}, {VK_OEM_6, "right_bracket"}, {VK_OEM_MINUS, "minus"}, {VK_OEM_PLUS, "equals"},
    {VK_F5, "f5"}, {VK_F6, "f6"}, {VK_F7, "f7"}, {VK_F8, "f8"}, {VK_F9, "f9"}, {VK_F10, "f10"}, {VK_F11, "f11"},
    {VK_F12, "f12"}};

}  // namespace

std::string KeyToName(uint16_t vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(std::tolower(vk)));
    for (const KeyName& k : kKeyNames)
        if (k.vk == vk) return k.name;
    return vk ? "vk" + std::to_string(vk) : "";
}

uint16_t KeyFromName(const std::string& name) {
    if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0])))
        return uint16_t(std::toupper(static_cast<unsigned char>(name[0])));
    for (const KeyName& k : kKeyNames)
        if (name == k.name) return k.vk;
    if (name.rfind("vk", 0) == 0) return uint16_t(std::atoi(name.c_str() + 2));
    return 0;
}

// arrows steer and move through menus (the left stick does both), W and S accelerate and brake, Enter or Space is A
std::array<uint16_t, 2> DefaultKeys(int c) {
    switch (c) {
        case input::kUp: return {VK_NUMPAD8, 0};
        case input::kDown: return {VK_NUMPAD2, 0};
        case input::kLeft: return {VK_NUMPAD4, 0};
        case input::kRight: return {VK_NUMPAD6, 0};
        case input::kStart: return {VK_ESCAPE, 'P'};
        case input::kBack: return {VK_TAB, 0};
        case input::kLThumb: return {'C', 0};
        case input::kRThumb: return {'V', 0};
        case input::kA: return {VK_RETURN, VK_SPACE};
        case input::kB: return {VK_BACK, 0};
        case input::kX: return {'Q', 0};
        case input::kY: return {'E', 0};
        case input::kBlack: return {'R', 0};
        case input::kWhite: return {'F', 0};
        case input::kLT: return {'S', 0};
        case input::kRT: return {'W', 0};
        case input::kLSLeft: return {VK_LEFT, 'A'};
        case input::kLSRight: return {VK_RIGHT, 'D'};
        case input::kLSUp: return {VK_UP, 0};
        case input::kLSDown: return {VK_DOWN, 0};
        case input::kRSLeft: return {'J', 0};
        case input::kRSRight: return {'L', 0};
        case input::kRSUp: return {'I', 0};
        case input::kRSDown: return {'K', 0};
        default: return {0, 0};
    }
}

Settings& Get() {
    // the control tables have their size from the start: the settings window changes entries, game threads read them
    static Settings s = [] {
        Settings d;
        d.keys.resize(input::kControls);
        d.pad_map.resize(input::kControls);
        for (int c = 0; c < input::kControls; c++) {
            d.keys[c] = DefaultKeys(c);
            d.pad_map[c] = uint8_t(c);
        }
        return d;
    }();
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    std::string aspect = t["aspect"].value_or(std::string(kAspects[s.aspect % kAspectCount]));
    for (uint32_t i = 0; i < kAspectCount; i++)
        if (aspect == kAspects[i]) s.aspect = i;
    s.keyboard = t["keyboard"].value_or(s.keyboard);
    s.monitor = t["monitor"].value_or(s.monitor);
    s.pause_unfocused = t["pause_unfocused"].value_or(s.pause_unfocused);
    s.show_fps = t["show_fps"].value_or(s.show_fps);
    std::string aa = t["antialiasing"].value_or(std::string(kAntialiasing[s.antialiasing % kAntialiasingCount]));
    for (uint32_t i = 0; i < kAntialiasingCount; i++)
        if (aa == kAntialiasing[i]) s.antialiasing = i;
    std::string renderer = t["renderer"].value_or(std::string(kRenderers[s.renderer % kRendererCount]));
    for (uint32_t i = 0; i < kRendererCount; i++)
        if (renderer == kRenderers[i]) s.renderer = i;
    // [keys] input = ["key", "key"], [gamepad] input = "physical input"
    if (const toml::table* keys = t["keys"].as_table()) {
        for (auto& [name, v] : *keys) {
            int c = input::ControlByName(std::string(name.str()).c_str());
            const toml::array* a = v.as_array();
            if (c < 0 || !a) continue;
            s.keys[c] = {0, 0};
            for (size_t i = 0; i < 2 && i < a->size(); i++) s.keys[c][i] = KeyFromName((*a)[i].value_or(std::string()));
        }
    }
    if (const toml::table* pad = t["gamepad"].as_table()) {
        for (auto& [name, v] : *pad) {
            int c = input::ControlByName(std::string(name.str()).c_str());
            int from = input::ControlByName(v.value_or(std::string()).c_str());
            if (c >= 0 && from >= 0) s.pad_map[c] = uint8_t(from);
        }
    }
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
}

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"join_ip", s.join_ip},
                  {"aspect", kAspects[s.aspect % kAspectCount]},
                  {"keyboard", s.keyboard},
                  {"monitor", int64_t(s.monitor)},
                  {"pause_unfocused", s.pause_unfocused},
                  {"show_fps", s.show_fps},
                  {"antialiasing", kAntialiasing[s.antialiasing % kAntialiasingCount]},
                  {"renderer", kRenderers[s.renderer % kRendererCount]}};
    // only what differs from the defaults
    toml::table keys, pad;
    for (int c = 0; c < input::kControls; c++) {
        if (c < int(s.keys.size()) && s.keys[c] != DefaultKeys(c))
            keys.insert(input::ControlName(c), toml::array{KeyToName(s.keys[c][0]), KeyToName(s.keys[c][1])});
        if (c < int(s.pad_map.size()) && s.pad_map[c] != c) pad.insert(input::ControlName(c), input::ControlName(s.pad_map[c]));
    }
    if (!keys.empty()) t.insert("keys", keys);
    if (!pad.empty()) t.insert("gamepad", pad);
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

}  // namespace

void Save() {
    std::ofstream f(File());
    f << Table() << "\n";
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

void* Monitor() {
    uint32_t n = Get().monitor;
    if (n) {
        std::vector<HMONITOR> list = MonitorList();
        if (n <= list.size()) return list[n - 1];
    }
    return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

std::vector<std::string> Monitors() {
    std::vector<std::string> names;
    for (HMONITOR m : MonitorList()) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(m, &mi);
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        std::wstring name = EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0) ? dd.DeviceString : mi.szDevice;
        int w = mi.rcMonitor.right - mi.rcMonitor.left, h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        std::string text(name.begin(), name.end());  // device names are ASCII
        names.push_back(std::to_string(names.size() + 1) + ": " + text + " (" + std::to_string(w) + " x " +
                        std::to_string(h) + (mi.dwFlags & MONITORINFOF_PRIMARY ? ", primary)" : ")"));
    }
    return names;
}

Mode Desktop() {
    HMONITOR mon = static_cast<HMONITOR>(Monitor());
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
        return {dm.dmPelsWidth, dm.dmPelsHeight};
    return {uint32_t(GetSystemMetrics(SM_CXSCREEN)), uint32_t(GetSystemMetrics(SM_CYSCREEN))};
}

Mode Output() {
    const Settings& s = Get();
    if (s.width && s.height) return {s.width, s.height};
    return Desktop();
}

bool Widescreen() {
    // asked per frame and per HUD quad: the desktop mode lookup only runs when the resolution setting changes
    static std::atomic<uint64_t> key{~0ull};
    static std::atomic<bool> wide{true};
    const Settings& s = Get();
    if (s.aspect == 1) return false;
    if (s.aspect >= 2) return true;
    uint64_t k = (uint64_t(s.width) << 32 | s.height) ^ uint64_t(s.monitor) << 60;
    if (key.exchange(k) != k) {
        Mode m = Output();
        wide = m.width * 2 >= m.height * 3;  // 3:2 or wider
    }
    return wide;
}

float FrameAspect() {
    if (!Widescreen()) return 4.0f / 3.0f;
    if (Get().aspect != 3) return 16.0f / 9.0f;
    Mode m = Output();
    return m.height ? std::max(16.0f / 9.0f, float(m.width) / float(m.height)) : 16.0f / 9.0f;
}

float RenderScale(uint32_t guest_height) {
    float h = Output().height * (Get().render_percent / 100.0f);
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

}  // namespace app
