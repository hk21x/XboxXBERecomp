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
    ImGui::TextDisabled("Click a key box, then press the key (Escape clears it). Each input takes up to two keys.");
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
