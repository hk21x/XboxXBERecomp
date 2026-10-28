// the in-game settings window, F1: display, graphics, the game's own options and controls. Changes are saved to the
// settings file at once; display mode, resolution and render scale apply at once, aspect at the next start.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"

namespace app {

namespace {

std::atomic<bool> g_open{false};
// a key binding waiting for its key: the input and the slot, -1 none
std::atomic<int> g_bind_control{-1}, g_bind_slot{0};

// the monitor's modes, widest first, each size once
std::vector<Mode> Modes() {
    std::vector<Mode> modes;
    HMONITOR mon = static_cast<HMONITOR>(Monitor());
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return modes;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &dm); i++) {
        if (dm.dmBitsPerPel < 32 || dm.dmPelsHeight < 480) continue;
        Mode m{dm.dmPelsWidth, dm.dmPelsHeight};
        if (std::none_of(modes.begin(), modes.end(), [&](const Mode& o) { return o.width == m.width && o.height == m.height; }))
            modes.push_back(m);
    }
    std::sort(modes.begin(), modes.end(), [](const Mode& a, const Mode& b) {
        return a.width != b.width ? a.width > b.width : a.height > b.height;
    });
    return modes;
}

void Display(Settings& s) {
    bool changed = false;
    static std::vector<std::string> monitors = Monitors();
    if (monitors.size() > 1) {
        std::string current = s.monitor && s.monitor <= monitors.size() ? monitors[s.monitor - 1] : "Primary display";
        if (ImGui::BeginCombo("Monitor", current.c_str())) {
            if (ImGui::Selectable("Primary display", s.monitor == 0)) {
                s.monitor = 0;
                changed = true;
            }
            for (uint32_t i = 0; i < monitors.size(); i++)
                if (ImGui::Selectable(monitors[i].c_str(), s.monitor == i + 1)) {
                    s.monitor = i + 1;
                    changed = true;
                }
            ImGui::EndCombo();
        }
    }
    const char* modes[] = {"Fullscreen", "Borderless window", "Window"};
    int mode = int(s.display_mode);
    if (ImGui::Combo("Display mode", &mode, modes, 3)) {
        s.display_mode = DisplayMode(mode);
        changed = true;
    }
    static uint32_t list_monitor = ~0u;
    static std::vector<Mode> list;
    if (list_monitor != s.monitor) list = Modes(), list_monitor = s.monitor;
    std::string current = s.width && s.height ? std::to_string(s.width) + " x " + std::to_string(s.height) : "Desktop";
    if (ImGui::BeginCombo("Resolution", current.c_str())) {
        if (ImGui::Selectable("Desktop", !s.width)) {
            s.width = s.height = 0;
            changed = true;
        }
        for (const Mode& m : list) {
            std::string name = std::to_string(m.width) + " x " + std::to_string(m.height);
            if (ImGui::Selectable(name.c_str(), s.width == m.width && s.height == m.height)) {
                s.width = m.width;
                s.height = m.height;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::Checkbox("Vertical sync", &s.vsync)) Save();
    if (changed) {
        Save();
        ApplyDisplay();
    }
    int aspect = int(s.aspect % 4);
    const char* aspects[] = {"From the screen's shape", "4:3", "16:9 (widescreen)", "Fill the screen (ultrawide)"};
    if (ImGui::Combo("Aspect ratio", &aspect, aspects, 4)) {
        s.aspect = uint32_t(aspect);
        Save();
    }
    ImGui::TextDisabled("Aspect ratio applies at the next start (the game reads its TV setting when it boots).");
}

void Graphics(Settings& s) {
    int percent = int(s.render_percent);
    if (ImGui::SliderInt("Render scale (% of the screen)", &percent, 25, 200)) {
        s.render_percent = uint32_t(percent);
        Save();
    }
    ImGui::TextDisabled("Render scale applies at once.");
    const int anisos[] = {0, 2, 4, 8, 16};
    const char* aniso_names[] = {"As the game asks", "2x", "4x", "8x", "16x"};
    int a = 0;
    for (int i = 0; i < 5; i++)
        if (int(s.aniso) == anisos[i]) a = i;
    if (ImGui::Combo("Anisotropic filtering", &a, aniso_names, 5)) {
        s.aniso = uint32_t(anisos[a]);
        Save();
    }
    const int caps[] = {30, 60, 120, 144, 240, 0};
    const char* cap_names[] = {"30", "60", "120", "144", "240", "Unlimited"};
    int c = 1;
    for (int i = 0; i < 6; i++)
        if (int(s.fps_cap) == caps[i]) c = i;
    if (ImGui::Combo("Frame rate cap", &c, cap_names, 6)) {
        s.fps_cap = uint32_t(caps[c]);
        Save();
    }
    ImGui::TextDisabled("The game sets its own pace (menus 60, races 30); the cap only limits it further.");
    int aa = int(s.antialiasing % 5);
    const char* aa_names[] = {"Off", "FXAA (smooths edges of the finished frame)", "MSAA 2x", "MSAA 4x", "MSAA 8x"};
    if (ImGui::Combo("Antialiasing", &aa, aa_names, 5)) {
        s.antialiasing = uint32_t(aa);
        Save();
    }
    ImGui::TextDisabled("MSAA smooths the edges of the 3D scene at once; render scale above 100%% also smooths edges.");
    if (ImGui::Checkbox("Show frames a second", &s.show_fps)) Save();
    if (kGame.sharpen_2d && ImGui::Checkbox("Sharp text and HUD", &s.sharp_2d)) Save();
    int renderer = int(s.renderer % 2);
    const char* renderers[] = {"Direct3D 12", "Direct3D 11"};
    if (ImGui::Combo("Renderer", &renderer, renderers, 2)) {
        s.renderer = uint32_t(renderer);
        Save();
    }
    ImGui::TextDisabled("Renderer applies at the next start; Direct3D 11 for GPUs or drivers where 12 runs badly.");
}

void General(Settings& s) {
    if (ImGui::Checkbox("Pause when the window is not in front", &s.pause_unfocused)) Save();
    ImGui::TextDisabled("The game and its sound stop while another window is in front, and carry on when you return.");
}

void GameOptions() {
    if (kGame.options.empty()) {
        ImGui::TextDisabled("No options for this game.");
        return;
    }
    for (const Option& o : kGame.options) {
        const char* name = o.label ? o.label : o.key;
        if (o.flag) {
            if (ImGui::Checkbox(name, o.flag)) Save();
        } else if (o.value) {
            int v = int(*o.value);
            if (ImGui::InputInt(name, &v)) {
                *o.value = uint32_t(std::max(v, 0));
                Save();
            }
        }
        if (o.help) ImGui::TextDisabled("%s", o.help);
    }
}

// the wheel and the rest of a rig (core/wheel.cpp): the devices found, and each control bound by moving or pressing it
// on any of them (the binding names the device)
const char* kAxisNames[8] = {"X", "Y", "Z", "Rx", "Ry", "Rz", "Slider 1", "Slider 2"};
struct WheelDetect {
    enum Kind { None, Steer, Pedal, Button } kind = None;
    Settings::WheelBind* bind = nullptr;
    std::vector<input::WheelDevice> start;  // every device's state when it began
} g_detect;

std::string BindText(const Settings::WheelBind& b) {
    if (b.axis < 0 && b.button < 0) return "-";
    std::string dev = b.device.empty() ? "the wheel" : b.device;
    if (b.axis >= 0) return dev + ": " + kAxisNames[std::clamp(b.axis, 0, 7)] + " axis";
    return dev + ": button " + std::to_string(b.button);
}

void Bound(Settings::WheelBind& b, const input::WheelDevice& d) {
    b.device = d.name;
    b.guid = d.guid;
    Save();
    input::WheelSetup();
}

// what moved or was pressed since the detection began, on any device
void Detect(const input::WheelInfo& w) {
    if (g_detect.kind == WheelDetect::None) return;
    for (size_t i = 0; i < w.devices.size() && i < g_detect.start.size(); i++) {
        const input::WheelDevice& d = w.devices[i];
        const input::WheelDevice& was = g_detect.start[i];
        if (d.guid != was.guid) continue;
        if (g_detect.kind == WheelDetect::Button) {
            for (int b : d.pressed)
                if (std::find(was.pressed.begin(), was.pressed.end(), b) == was.pressed.end()) {
                    *g_detect.bind = {.button = b};
                    Bound(*g_detect.bind, d);
                    g_detect = {};
                    return;
                }
            continue;
        }
        int32_t need = g_detect.kind == WheelDetect::Steer ? 8000 : 20000;
        for (int a = 0; a < 8; a++) {
            if (!(d.axis_mask >> a & 1)) continue;  // the axes the device has, wherever they are (Z, Rz, a slider ...)
            int32_t moved = d.axes[a] - was.axes[a];
            if (std::abs(moved) <= need) continue;
            Settings::WheelBind& b = *g_detect.bind;
            if (g_detect.kind == WheelDetect::Steer) {
                b = {.axis = a, .invert = moved < 0};  // turned right
            } else {
                b = {.axis = a, .rest = was.axes[a], .full = moved > 0 ? 32767 : -32768};
            }
            Bound(b, d);
            g_detect = {};
            return;
        }
    }
}

// a binding's button: click, then move or press the control; "clear" unbinds it
void BindButton(const char* id, Settings::WheelBind& b, WheelDetect::Kind kind, const input::WheelInfo& w,
                const char* prompt) {
    ImGui::PushID(id);
    bool waiting = g_detect.kind == kind && g_detect.bind == &b;
    std::string text = waiting ? prompt : BindText(b);
    if (ImGui::Button(text.c_str(), ImVec2(ImGui::GetFontSize() * 18, 0))) {
        g_detect = {.kind = kind, .bind = &b, .start = w.devices};
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("clear")) {
        b = {};
        Save();
        input::WheelSetup();
    }
    ImGui::PopID();
}

void Wheel(Settings& s) {
    input::WheelInfo w = input::Wheel();
    Detect(w);
    ImGui::SeparatorText("Devices");
    if (w.devices.empty()) {
        ImGui::TextUnformatted("No wheel, pedals, shifter or button box found.");
        ImGui::TextDisabled("Every DirectInput game controller is listed here (Xbox pads are under Controls).");
        ImGui::TextDisabled("One plugged in later is found within two seconds.");
        g_detect = {};
        return;
    }
    for (const input::WheelDevice& d : w.devices)
        ImGui::BulletText("%s: %d axes, %d buttons, %d hats%s%s", d.name.c_str(), d.axes_count, d.buttons_count,
                          d.povs_count, d.ffb ? ", force feedback" : "", d.feedback ? " (in use)" : "");
    ImGui::TextDisabled("Bind each control below by clicking it, then turning, pressing or moving it on any device.");

    ImGui::SeparatorText("Steering");
    float degrees = w.steer * float(s.wheel_lock) * 0.5f;
    ImGui::ProgressBar(0.5f + w.steer * 0.5f, ImVec2(-1, 0), (std::to_string(int(std::lround(degrees))) + " degrees").c_str());
    BindButton("steer", s.wheel_steer, WheelDetect::Steer, w, "turn the wheel to the right...");
    ImGui::SameLine();
    if (ImGui::Checkbox("Invert", &s.wheel_steer.invert)) Save(), input::WheelSetup();
    int range = int(s.wheel_range), lock = int(s.wheel_lock), dz = int(s.wheel_deadzone);
    if (ImGui::SliderInt("Wheel rotation (degrees, as set in the wheel's software)", &range, 180, 2880))
        s.wheel_range = uint32_t(range), Save(), input::WheelSetup();
    if (ImGui::SliderInt("Steering lock (degrees for full lock)", &lock, 90, 1080))
        s.wheel_lock = uint32_t(lock), Save(), input::WheelSetup();
    if (ImGui::SliderInt("Dead zone (%)", &dz, 0, 20)) s.wheel_deadzone = uint32_t(dz), Save(), input::WheelSetup();
    ImGui::TextDisabled("Steering lock: a smaller value turns the car fully with less wheel (360 suits most cars).");

    ImGui::SeparatorText("Pedals");
    struct Row {
        const char* name;
        Settings::WheelBind* bind;
        float value;
    } rows[3] = {{"Throttle", &s.wheel_throttle, w.throttle}, {"Brake", &s.wheel_brake, w.brake},
                 {"Clutch (unused by the game)", &s.wheel_clutch, w.clutch}};
    for (Row& r : rows) {
        ImGui::ProgressBar(r.value, ImVec2(ImGui::GetFontSize() * 10, 0), r.name);
        ImGui::SameLine();
        BindButton(r.name, *r.bind, WheelDetect::Pedal, w, "press the pedal fully...");
    }
    ImGui::TextDisabled("Let the pedal rest, click, then press it all the way.");

    ImGui::SeparatorText("Force feedback");
    std::string on;
    for (const input::WheelDevice& d : w.devices)
        if (d.feedback) on = d.name;
    if (on.empty()) ImGui::TextDisabled("None: the device the steering is bound to offers none (or another program has it).");
    else ImGui::TextDisabled("On %s: %s", on.c_str(), w.effects.empty() ? "no effects made" : w.effects.c_str());
    int strength = int(s.ffb_strength), road = int(s.ffb_road);
    if (ImGui::SliderInt("Strength (%)", &strength, 0, 100)) s.ffb_strength = uint32_t(strength), Save(), input::WheelSetup();
    if (ImGui::SliderInt("Road and impacts (%)", &road, 0, 100)) s.ffb_road = uint32_t(road), Save(), input::WheelSetup();
    if (ImGui::Button("Push left")) input::WheelTestForce(-0.6f);
    ImGui::SameLine();
    if (ImGui::Button("Push right")) input::WheelTestForce(0.6f);
    ImGui::TextDisabled("Centring firms up with speed; kerbs, gravel and impacts shake the wheel.");

    ImGui::SeparatorText("Buttons");
    if (ImGui::Button("Default buttons (Logitech G920/G923)")) {
        for (int c = 0; c < input::kControls; c++) s.wheel_buttons[c] = {.button = DefaultWheelButton(c)};
        Save();
        input::WheelSetup();
    }
    if (ImGui::BeginTable("wheel_buttons", 2, ImGuiTableFlags_RowBg)) {
        // the d-pad first (a wheel without a hat binds it to buttons), then the buttons
        for (int c = input::kUp; c <= input::kWhite; c++) {
            if (c == input::kLT || c == input::kRT) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(input::ControlLabel(c));
            ImGui::TableNextColumn();
            BindButton(input::ControlName(c), s.wheel_buttons[c], WheelDetect::Button, w, "press a button...");
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("The steering device's hat is the d-pad too, with any d-pad buttons bound here.");

    ImGui::SeparatorText("H-pattern shifter");
    for (int i = 0; i < kGears; i++) {
        ImGui::Text("%s", i == 0 ? "Reverse" : ("Gear " + std::string(GearName(i))).c_str());
        ImGui::SameLine(ImGui::GetFontSize() * 5);
        BindButton(GearName(i), s.wheel_gears[i], WheelDetect::Button, w, "select the gear...");
    }
    auto gear = [](int g) {
        return g == input::kGearUnknown ? std::string("-") : g < 0 ? std::string("R") : g == 0 ? std::string("N") : std::to_string(g);
    };
    ImGui::TextDisabled("Shifter %s, car %s. With a manual gearbox the game's shift up and down are pressed until they match.",
                        gear(w.gear_selected).c_str(), gear(w.gear_game).c_str());

    ImGui::SeparatorText("Raw input");
    for (const input::WheelDevice& d : w.devices) {
        if (!ImGui::TreeNode(d.guid.c_str(), "%s", d.name.c_str())) continue;
        for (int a = 0; a < 8; a++)
            if (d.axis_mask >> a & 1) ImGui::ProgressBar((d.axes[a] + 32768) / 65535.0f, ImVec2(-1, 0),
                               (std::string(kAxisNames[a]) + " " + std::to_string(d.axes[a])).c_str());
        std::string held;
        for (int b : d.pressed) held += (held.empty() ? "" : " ") + std::to_string(b);
        ImGui::TextDisabled("Buttons held: %s%s", held.empty() ? "none" : held.c_str(),
                            d.pov >= 0 ? (", hat " + std::to_string(d.pov / 100)).c_str() : "");
        ImGui::TreePop();
    }
}

void Controls(Settings& s) {
    if (s.keys.size() != input::kControls || s.pad_map.size() != input::kControls) return;  // sized in Get
    if (ImGui::Checkbox("Keyboard plays as controller 1", &s.keyboard)) Save();
    ImGui::SameLine();
    if (ImGui::Button("Default keys")) {
        for (int c = 0; c < input::kControls; c++) s.keys[c] = DefaultKeys(c);
        Save();
    }
    ImGui::SameLine();
    if (ImGui::Button("Default controller")) {
        for (int c = 0; c < input::kControls; c++) s.pad_map[c] = uint8_t(c);
        Save();
    }
    if (kGame.mouse_look) {
        if (ImGui::Checkbox("Mouse moves the right stick", &s.mouse_look)) Save();
        ImGui::SameLine();
        if (ImGui::Checkbox("Invert mouse up and down", &s.mouse_invert_y)) Save();
        int sens = int(s.mouse_sensitivity);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
        if (ImGui::SliderInt("Mouse sensitivity (%)", &sens, 10, 1000)) {
            s.mouse_sensitivity = uint32_t(sens);
            Save();
        }
    }
    ImGui::TextDisabled("Click a key box, then press the key or mouse button (Escape clears it). Each input takes up to two keys.");
    ImGui::TextDisabled("On a controller, Back + Start opens this window and Start closes it.");
    if (!ImGui::BeginTable("controls", 4, ImGuiTableFlags_RowBg)) return;
    // the names and keys at their full width, the controller input takes what is left
    float pad = ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().CellPadding.x * 2;
    ImGui::TableSetupColumn("Console input", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Right stick right").x + pad);
    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("press a key...").x + pad);
    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("press a key...").x + pad);
    ImGui::TableSetupColumn("Controller input", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (int c = 0; c < input::kControls; c++) {
        ImGui::PushID(c);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(input::ControlLabel(c));
        for (int slot = 0; slot < 2; slot++) {
            ImGui::TableNextColumn();
            ImGui::PushID(slot);
            bool waiting = g_bind_control == c && g_bind_slot == slot;
            std::string name = waiting ? "press a key..." : KeyToName(s.keys[c][slot]);
            if (name.empty()) name = "-";
            if (ImGui::Button(name.c_str(), ImVec2(-1, 0))) {
                g_bind_slot = slot;
                g_bind_control = c;
            }
            ImGui::PopID();
        }
        ImGui::TableNextColumn();
        int from = s.pad_map[c];
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##pad", input::ControlLabel(from))) {
            for (int p = 0; p < input::kControls; p++)
                if (ImGui::Selectable(input::ControlLabel(p), p == from)) {
                    s.pad_map[c] = uint8_t(p);
                    Save();
                }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

}  // namespace

bool OverlayOpen() { return g_open; }

void FeedPad() {
    using input::Control;
    struct Map {
        Control c;
        ImGuiKey key;
    };
    static const Map kMap[] = {
        {input::kUp, ImGuiKey_GamepadDpadUp},       {input::kDown, ImGuiKey_GamepadDpadDown},
        {input::kLeft, ImGuiKey_GamepadDpadLeft},   {input::kRight, ImGuiKey_GamepadDpadRight},
        {input::kLSUp, ImGuiKey_GamepadLStickUp},   {input::kLSDown, ImGuiKey_GamepadLStickDown},
        {input::kLSLeft, ImGuiKey_GamepadLStickLeft}, {input::kLSRight, ImGuiKey_GamepadLStickRight},
        {input::kA, ImGuiKey_GamepadFaceDown},      {input::kB, ImGuiKey_GamepadFaceRight},
        {input::kX, ImGuiKey_GamepadFaceLeft},      {input::kY, ImGuiKey_GamepadFaceUp},
        {input::kWhite, ImGuiKey_GamepadL1},        {input::kBlack, ImGuiKey_GamepadR1},
        {input::kLT, ImGuiKey_GamepadL2},           {input::kRT, ImGuiKey_GamepadR2},
    };
    static uint32_t last = 0;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    uint32_t held = g_open ? input::HeldControls() : 0;
    for (const Map& m : kMap)
        if (((held ^ last) >> m.c) & 1) io.AddKeyEvent(m.key, (held >> m.c) & 1);
    last = held;
}

// the newest unlock for a few seconds, top right
void DrawUnlock() {
    std::string title;
    uint32_t points = 0;
    double age = 0;
    if (!achievements::Recent(title, points, age) || age > 6) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 16, vp->WorkPos.y + 16), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.8f);
    ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##unlock", nullptr, f)) {
        ImGui::TextDisabled("Achievement unlocked");
        ImGui::Text("%s  (%u)", title.c_str(), points);
    }
    ImGui::End();
}

void Achievements() {
    std::vector<achievements::Status> list = achievements::List();
    uint32_t got = 0, points = 0, total = 0;
    for (auto& a : list) {
        total += a.def->points;
        if (a.unlocked) got++, points += a.def->points;
    }
    ImGui::Text("%u of %zu unlocked, %u of %u points", got, list.size(), points, total);
    if (!ImGui::BeginTable("achievements", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("Achievement");
    ImGui::TableSetupColumn("Points");
    ImGui::TableSetupColumn("Unlocked");
    ImGui::TableHeadersRow();
    for (auto& a : list) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (a.unlocked) ImGui::TextUnformatted(a.def->title);
        else ImGui::TextDisabled("%s", a.def->title);
        ImGui::TextDisabled("%s", a.def->description);
        ImGui::TableNextColumn();
        ImGui::Text("%u", a.def->points);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(a.unlocked ? a.when.c_str() : "-");
    }
    ImGui::EndTable();
}

void DrawHud(double fps) {
    DrawUnlock();
    if (!Get().show_fps) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 8, vp->WorkPos.y + 8));
    ImGui::SetNextWindowBgAlpha(0.4f);
    ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##fps", nullptr, f)) ImGui::Text("%.0f fps", fps);
    ImGui::End();
}

void ToggleOverlay() {
    g_open = !g_open;
    g_bind_control = -1;
    if (ImGui::GetCurrentContext()) ImGui::GetIO().MouseDrawCursor = g_open;
    LOGR("settings window %s", g_open ? "opened" : "closed");
}

bool OverlayKey(uint16_t vk) {
    int c = g_bind_control;
    if (!g_open || c < 0) return false;
    Settings& s = Get();
    if (s.keys.size() == input::kControls) {
        s.keys[c][g_bind_slot] = vk == VK_ESCAPE ? 0 : vk;
        Save();
    }
    g_bind_control = -1;
    return true;
}

// Quit game, at the top right of the settings window, asks first: the window closes as with its own close button
// (Alt+F4 does the same)
static void QuitButton() {
    const char* label = "Quit game";
    ImGuiStyle& st = ImGui::GetStyle();
    float w = ImGui::CalcTextSize(label).x + st.FramePadding.x * 2;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - w);
    if (ImGui::Button(label)) ImGui::OpenPopup("Quit?");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Quit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Quit the game? Progress since the game last saved is lost.");
        if (ImGui::Button("Quit")) {
            LOGR("quit from the settings window");
            Quit();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
}

void DrawOverlay() {
    if (!g_open) return;
    Settings& s = Get();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    // readable on a big screen: the default font is sized for 720 lines
    ImGui::GetIO().FontGlobalScale = std::max(1.0f, vp->WorkSize.y / 720.0f);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.6f, vp->WorkSize.y * 0.8f),
                             ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::Begin("Settings (F1)", &open, ImGuiWindowFlags_NoCollapse)) {
        QuitButton();
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Display")) {
                Display(s);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Graphics")) {
                Graphics(s);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Game")) {
                GameOptions();
                ImGui::EndTabItem();
            }
            if (!kGame.achievements.empty() && ImGui::BeginTabItem("Achievements")) {
                Achievements();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("General")) {
                General(s);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Wheel")) {
                Wheel(s);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Controls")) {
                Controls(s);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    if (!open) ToggleOverlay();
}

}  // namespace app
