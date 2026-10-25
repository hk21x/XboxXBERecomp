// pad 0 from a script, then GameInfo::pad as on Windows
// SCRIPT file: lines of "vblank buttons [vblanks held]", held 8 by default, # comments; buttons joined by +. Times
// count vertical blanks at 60 a second, the frames the title asked for (a frame at 30 a second is two), so a script
// keeps its timing when a title changes its frame rate between screens:
// UP DOWN LEFT RIGHT START BACK A B X Y LT RT LS LSD LSL LSR (left stick up, down, left, right) RS RSD RSL RSR (right)
// MOUSEL MOUSER MOUSEU MOUSED (a mouse moving for input::TakeMouse), CLICKL CLICKR (its buttons held), WHEELU WHEELD
// (a notch of its wheel each poll held), KEYS (nothing pressed, on the keyboard: from then
// input::UsingKeyboard is true, as with the mouse tokens, until a controller's token)
// cmd in the run folder: "buttons [vblanks held]" pressed once from when it is read, then removed
#include <cstdio>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "harness.h"

namespace input {

namespace {

float g_mouse_x = 0, g_mouse_y = 0;  // this poll's scripted mouse, for input::TakeMouse
bool g_keyboard = false;             // input::UsingKeyboard
uint32_t g_buttons = 0;              // this poll's mouse buttons, for input::MouseButtons
int g_wheel = 0;                     // notches scripted and not yet taken (input::TakeWheel)

struct Press {
    double at;
    std::string buttons;
    double len;
};

std::vector<Press> LoadScript() {
    std::vector<Press> v;
    const char* p = std::getenv("SCRIPT");
    if (FILE* f = p ? std::fopen(p, "r") : nullptr) {
        char line[160];
        while (std::fgets(line, sizeof line, f)) {
            char b[64];
            double t, len = 8;
            if (line[0] != '#' && std::sscanf(line, "%lf %63s %lf", &t, b, &len) >= 2) v.push_back({t, b, len});
        }
        std::fclose(f);
        LOGI("harness: pad script %s, %zu presses", p, v.size());
    }
    return v;
}

void Apply(const std::string& s, Pad& p) {
    size_t at = 0;
    while (at <= s.size()) {
        size_t e = s.find('+', at);
        if (e == std::string::npos) e = s.size();
        std::string b = s.substr(at, e - at);
        at = e + 1;
        if (b == "KEYS" || b.rfind("MOUSE", 0) == 0 || b.rfind("CLICK", 0) == 0 || b.rfind("WHEEL", 0) == 0) g_keyboard = true;
        else if (!b.empty()) g_keyboard = false;
        if (b == "UP") p.buttons |= 1;
        else if (b == "DOWN") p.buttons |= 2;
        else if (b == "LEFT") p.buttons |= 4;
        else if (b == "RIGHT") p.buttons |= 8;
        else if (b == "START") p.buttons |= 16;
        else if (b == "BACK") p.buttons |= 32;
        else if (b == "A") p.analog[0] = 255;
        else if (b == "B") p.analog[1] = 255;
        else if (b == "X") p.analog[2] = 255;
        else if (b == "Y") p.analog[3] = 255;
        else if (b == "BLACK") p.analog[4] = 255;
        else if (b == "WHITE") p.analog[5] = 255;
        else if (b == "LT") p.analog[6] = 255;
        else if (b == "RT") p.analog[7] = 255;
        else if (b == "LS") p.ly = 32767;
        else if (b == "LSD") p.ly = -32767;
        else if (b == "LSL") p.lx = -32767;
        else if (b == "LSR") p.lx = 32767;
        else if (b == "RS") p.ry = 32767;
        else if (b == "RSD") p.ry = -32767;
        else if (b == "RSL") p.rx = -32767;
        else if (b == "RSR") p.rx = 32767;
        else if (b == "MOUSEL") g_mouse_x = -0.5f;  // input::TakeMouse: a mouse moving at half a deflection
        else if (b == "MOUSER") g_mouse_x = 0.5f;
        else if (b == "MOUSEU") g_mouse_y = 0.5f;
        else if (b == "MOUSED") g_mouse_y = -0.5f;
        else if (b == "CLICKL") g_buttons |= 1;
        else if (b == "CLICKR") g_buttons |= 2;
        else if (b == "WHEELU") g_wheel = 1;
        else if (b == "WHEELD") g_wheel = -1;
    }
}

}  // namespace

Pad Poll(int port) {
    static const std::vector<Press> script = LoadScript();
    static Press live{-1, "", 0};
    Pad p;
    p.connected = port == 0;
    if (port == 0) {
        g_mouse_x = g_mouse_y = 0;
        g_buttons = 0;
        double t = double(harness::g_vblanks.load());
        std::string cmd = (app::ExeDir() / "cmd").string();
        if (FILE* f = std::fopen(cmd.c_str(), "r")) {
            char b[64] = {};
            double len = 3;
            if (std::fscanf(f, "%63s %lf", b, &len) >= 1) live = {t, b, len};
            std::fclose(f);
            std::remove(cmd.c_str());
        }
        for (const Press& s : script)
            if (t >= s.at && t <= s.at + s.len) Apply(s.buttons, p);
        if (live.at >= 0 && t >= live.at && t <= live.at + live.len) Apply(live.buttons, p);
    }
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

// the script's mouse (MOUSEL MOUSER MOUSEU MOUSED) as of the last poll
bool TakeMouse(float& x, float& y) {
    x = g_mouse_x;
    y = g_mouse_y;
    return true;
}

bool UsingKeyboard() { return g_keyboard; }
uint32_t MouseButtons() { return g_buttons; }
// a held WHEELU or WHEELD gives one notch a poll
int TakeWheel() {
    int w = g_wheel;
    g_wheel = 0;
    return w;
}

// the title's defaults (no settings file headless)
std::array<uint16_t, 2> ControlKeys(int c) {
    return app::kGame.default_keys ? app::kGame.default_keys(c) : std::array<uint16_t, 2>{0, 0};
}

void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
}

}  // namespace input
