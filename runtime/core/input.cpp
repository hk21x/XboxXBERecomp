// controllers through SDL: Xbox, PlayStation and Switch pads read into the console controller's report
#include "core/input.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>

#include <SDL3/SDL.h>

#include "app/app.h"
#include "core/log.h"

namespace input {

namespace {

// a controller takes the lowest free port when it is plugged in and keeps it until it is unplugged
std::mutex g_lock;
bool g_sdl = false;
SDL_Gamepad* g_pads[4] = {};

void AddPad(SDL_JoystickID id) {
    for (SDL_Gamepad* pad : g_pads)
        if (pad && SDL_GetGamepadID(pad) == id) return;
    SDL_Gamepad** slot = std::find(std::begin(g_pads), std::end(g_pads), nullptr);
    if (slot == std::end(g_pads)) return;
    *slot = SDL_OpenGamepad(id);
    const char* name = *slot ? SDL_GetGamepadName(*slot) : SDL_GetError();
    LOGI("input: controller %d %s: %s", int(slot - g_pads) + 1, *slot ? "plugged in" : "failed to open", name ? name : "");
}

void RemovePad(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++) {
        if (!g_pads[i] || SDL_GetGamepadID(g_pads[i]) != id) continue;
        SDL_CloseGamepad(g_pads[i]);
        g_pads[i] = nullptr;
        LOGI("input: controller %d unplugged", i + 1);
    }
}

// once a millisecond at most, however many ports and threads read
void UpdatePads() {
    static uint64_t last = 0;
    uint64_t now = SDL_GetTicks();
    if (!g_sdl || now == last) return;
    last = now;
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) RemovePad(e.gdevice.which);
    }
}

}  // namespace

void Init() {
    // WGI reads the triggers apart and HIDAPI takes PlayStation and Switch pads. Reads come from the guest's threads,
    // so SDL's own thread takes the device notices.
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        LOGE("input: SDL failed to start, no controllers: %s", SDL_GetError());
        return;
    }
    int v = SDL_GetVersion();
    LOGI("input: SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    std::lock_guard lock(g_lock);
    g_sdl = true;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) AddPad(ids[i]);
        SDL_free(ids);
    }
}

namespace {
constexpr const char* kNames[kControls] = {
    "dpad_up", "dpad_down", "dpad_left", "dpad_right", "start", "back", "left_thumb", "right_thumb",
    "a", "b", "x", "y", "black", "white", "left_trigger", "right_trigger",
    "left_stick_left", "left_stick_right", "left_stick_up", "left_stick_down",
    "right_stick_left", "right_stick_right", "right_stick_up", "right_stick_down"};
constexpr const char* kLabels[kControls] = {
    "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Start", "Back", "Left stick click", "Right stick click",
    "A", "B", "X", "Y", "Black (RB)", "White (LB)", "Left trigger", "Right trigger",
    "Left stick left", "Left stick right", "Left stick up", "Left stick down",
    "Right stick left", "Right stick right", "Right stick up", "Right stick down"};

// a pad's inputs as 0 to 1 each, in Control order
void ReadPad(SDL_Gamepad* pad, float* v) {
    constexpr SDL_GamepadButton kButtons[] = {
        SDL_GAMEPAD_BUTTON_DPAD_UP,    SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
        // A B X Y black white
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
    for (int i = 0; i < 14; i++) v[i] = SDL_GetGamepadButton(pad, kButtons[i]) ? 1.0f : 0.0f;
    auto axis = [&](SDL_GamepadAxis a) { return float(SDL_GetGamepadAxis(pad, a)) / 32767.0f; };
    v[kLT] = std::max(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0.0f);
    v[kRT] = std::max(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0.0f);
    // SDL's sticks have y down
    float lx = axis(SDL_GAMEPAD_AXIS_LEFTX), ly = axis(SDL_GAMEPAD_AXIS_LEFTY);
    float rx = axis(SDL_GAMEPAD_AXIS_RIGHTX), ry = axis(SDL_GAMEPAD_AXIS_RIGHTY);
    v[kLSLeft] = std::max(-lx, 0.0f), v[kLSRight] = std::max(lx, 0.0f);
    v[kLSUp] = std::max(-ly, 0.0f), v[kLSDown] = std::max(ly, 0.0f);
    v[kRSLeft] = std::max(-rx, 0.0f), v[kRSRight] = std::max(rx, 0.0f);
    v[kRSUp] = std::max(-ry, 0.0f), v[kRSDown] = std::max(ry, 0.0f);
}

int16_t Stick(float neg, float pos) {
    float v = std::clamp(pos - neg, -1.0f, 1.0f);
    return int16_t(std::lround(v * 32767.0f));
}
std::atomic<uint32_t> g_held[4];  // HeldControls, per port
}  // namespace

const char* ControlName(int c) { return c >= 0 && c < kControls ? kNames[c] : ""; }
const char* ControlLabel(int c) { return c >= 0 && c < kControls ? kLabels[c] : ""; }
int ControlByName(const char* name) {
    for (int c = 0; c < kControls; c++)
        if (std::strcmp(kNames[c], name) == 0) return c;
    return -1;
}

uint32_t HeldControls() { return g_held[0] | g_held[1] | g_held[2] | g_held[3]; }

Pad Poll(int port) {
    Pad p;
    float in[kControls] = {}, out[kControls] = {};
    const app::Settings& s = app::Get();
    {
        std::lock_guard lock(g_lock);
        UpdatePads();
        if (SDL_Gamepad* pad = port >= 0 && port < 4 ? g_pads[port] : nullptr) {
            p.connected = true;
            ReadPad(pad, in);
        }
    }
    // remapped: each console input reads the physical input the settings name for it
    for (int c = 0; c < kControls; c++) {
        int from = c < int(s.pad_map.size()) && s.pad_map[c] < kControls ? s.pad_map[c] : c;
        out[c] = in[from];
    }
    // the keyboard is controller 1, plugged in while keyboard play is on (alt-tabbing away must not unplug it), its
    // keys read while the game window has the focus
    HWND hwnd = static_cast<HWND>(app::Window());
    if (port == 0 && s.keyboard) p.connected = true;
    if (port == 0 && s.keyboard && hwnd && GetForegroundWindow() == hwnd) {
        for (int c = 0; c < kControls; c++) {
            std::array<uint16_t, 2> keys = c < int(s.keys.size()) ? s.keys[c] : app::DefaultKeys(c);
            for (uint16_t k : keys)
                if (k && (GetAsyncKeyState(k) & 0x8000)) out[c] = 1.0f;
        }
    }
    // Back + Start on a controller opens or closes the settings window, Start alone closes it; what was held then
    // stays from the game until let go
    uint32_t held = 0;
    for (int c = 0; c < kControls; c++) held |= uint32_t(in[c] >= 0.5f) << c;
    if (port >= 0 && port < 4) {
        static std::atomic<uint32_t> last[4], swallow[4];
        g_held[port] = held;
        uint32_t pressed = held & ~last[port].exchange(held);
        constexpr uint32_t kStartBit = 1u << kStart, kBackBit = 1u << kBack;
        bool open = app::OverlayOpen();
        if ((pressed & kStartBit) && ((held & kBackBit) || open)) {
            std::lock_guard lock(app::UiLock());
            app::ToggleOverlay();
            for (auto& w : swallow) w = kStartBit | kBackBit;
        }
        swallow[port] &= held;
        for (int c = 0; c < kControls; c++)
            if (swallow[port] & (1u << (s.pad_map.size() > size_t(c) ? s.pad_map[c] : c))) out[c] = 0;
    }
    // the settings window takes the input while it is open
    if (app::OverlayOpen()) std::fill(std::begin(out), std::end(out), 0.0f);
    for (int i = 0; i < 8; i++) p.buttons |= uint16_t(out[i] >= 0.5f) << i;
    for (int i = 0; i < 8; i++) p.analog[i] = uint8_t(std::lround(std::clamp(out[kA + i], 0.0f, 1.0f) * 255.0f));
    p.lx = Stick(out[kLSLeft], out[kLSRight]);
    p.ly = Stick(out[kLSDown], out[kLSUp]);
    p.rx = Stick(out[kRSLeft], out[kRSRight]);
    p.ry = Stick(out[kRSDown], out[kRSUp]);
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

// the motors run until set again
void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    std::lock_guard lock(g_lock);
    if (port >= 0 && port < 4 && g_pads[port]) SDL_RumbleGamepad(g_pads[port], left, right, 0);
}

}  // namespace input
