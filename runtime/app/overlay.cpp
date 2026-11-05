// the in-game settings window, F1: display, graphics, the game's own options and controls. Changes are saved to the
// settings file at once; display mode, resolution and render scale apply at once, aspect at the next start.
// A section list on the left and one full-width row per setting, in classic Steam's dark olive. Made for a controller:
// LB/RB change section, up/down rows, left/right change a row's value, A presses, Y clears a binding, B or Start closes.
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
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>

#include "app/app.h"
#include "app/classic_theme.h"
#include "core/input.h"
#include "core/log.h"

namespace app {

namespace {

std::atomic<bool> g_open{false};
// a key binding waiting for its key: the input and the slot, -1 none
std::atomic<int> g_bind_control{-1}, g_bind_slot{0};

// the frame counter and the unlock pop-up share the style
using namespace classic;
constexpr ImGuiKey kClearKey = ImGuiKey_GamepadBack;  // a controller's Y, see FeedPad
constexpr float kBaseFont = 15;  // pixels at 720 lines; the window scales with the screen

ImFont* g_bold = nullptr;
ImGuiStyle g_style;
float g_scale = 0;
const char* g_help = nullptr;   // the selected row's line for the footer
bool g_focus_first = false;     // select the page's first row
bool g_prompt_clear = false;    // the selected row is a binding: the footer offers Y
bool g_prompt_change = false;   // the selected row changes with left and right

void Theme() {
    ImGuiStyle s;
    s.WindowPadding = ImVec2(0, 0);
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(6, 2);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.CellPadding = ImVec2(4, 1);
    s.ScrollbarSize = 14;
    s.GrabMinSize = 12;
    s.DisplaySafeAreaPadding = ImVec2(0, 0);
    Apply(s);
    g_style = s;
}

// the type and the spacing follow the screen; the first frame sets the style up
void Scale() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float k = std::max(1.0f, vp->WorkSize.y / 720.0f);
    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = kBaseFont * k / io.Fonts->Fonts[0]->FontSize;
    if (k == g_scale) return;
    if (!g_scale) Theme();
    g_scale = k;
    ImGuiStyle s = g_style;
    s.ScaleAllSizes(k);
    ImGui::GetStyle() = s;
}

void SetHelp(const char* help) { g_help = help; }

// the arrows on any of a controller or the keyboard, repeating while held; held a second a press counts for `fast`
int LeftRight(bool repeat, int fast = 1) {
    int dir = 0;
    float held = 0;
    auto press = [&](ImGuiKey key, int d) {
        if (ImGui::IsKeyPressed(key, repeat)) dir = d;
        if (ImGui::IsKeyDown(key)) held = std::max(held, ImGui::GetKeyData(key)->DownDuration);
    };
    press(ImGuiKey_LeftArrow, -1), press(ImGuiKey_GamepadDpadLeft, -1);
    press(ImGuiKey_RightArrow, 1), press(ImGuiKey_GamepadDpadRight, 1);
    return held > 1 ? dir * fast : dir;
}

// one setting's row: the label on the left, the value's area on the right half; selected, it takes the accent
struct Row {
    ImRect rect, value;
    bool pressed, hovered, selected;
    ImU32 text;
};

Row RowBegin(const char* label, const char* help) {
    float w = ImGui::GetContentRegionAvail().x;
    float h = ImGui::GetFrameHeight() + ImGui::GetFontSize() * 0.3f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    Row r;
    r.pressed = ImGui::InvisibleButton(label, ImVec2(w, h));
    r.hovered = ImGui::IsItemHovered();
    r.selected = ImGui::IsItemFocused();
    if (g_focus_first) {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        g.NavDisableHighlight = false;
        g_focus_first = false;
        r.selected = true;
    }
    r.rect = ImRect(pos, ImVec2(pos.x + w, pos.y + h));
    float pad = ImGui::GetStyle().FramePadding.x;
    r.value = ImRect(ImVec2(pos.x + w * 0.5f, pos.y + 2), ImVec2(pos.x + w - pad, pos.y + h - 2));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (r.selected) dl->AddRectFilled(r.rect.Min, r.rect.Max, kSelect);
    else if (r.hovered) dl->AddRectFilled(r.rect.Min, r.rect.Max, kHover);
    r.text = r.selected ? kWhite : kText;
    const char* end = ImGui::FindRenderedTextEnd(label);
    dl->AddText(ImVec2(pos.x + pad, pos.y + (h - ImGui::GetFontSize()) * 0.5f), r.text, label, end);
    if (r.selected || r.hovered) SetHelp(help);
    return r;
}

// a sunken field, as the value of a row
void Field(const ImRect& r) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(r.Min, r.Max, kField);
    dl->AddLine(r.Min, ImVec2(r.Max.x, r.Min.y), kDark);
    dl->AddLine(r.Min, ImVec2(r.Min.x, r.Max.y), kDark);
    dl->AddLine(ImVec2(r.Min.x, r.Max.y - 1), ImVec2(r.Max.x, r.Max.y - 1), kLight);
    dl->AddLine(ImVec2(r.Max.x - 1, r.Min.y), ImVec2(r.Max.x - 1, r.Max.y), kLight);
}

void ValueText(const Row& r, const char* text, float align = 0.5f) {
    ImVec2 size = ImGui::CalcTextSize(text);
    ImVec2 pos(r.value.Min.x + (r.value.GetWidth() - size.x) * align, r.value.Min.y + (r.value.GetHeight() - size.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(ImVec2(std::max(pos.x, r.value.Min.x), pos.y), kText, text);
}

bool RowCheck(const char* label, bool* v, const char* help = nullptr) {
    Row r = RowBegin(label, help);
    bool changed = r.pressed;
    if (r.selected) g_prompt_change = true;
    if (r.selected && LeftRight(false)) changed = true;
    if (changed) *v = !*v;
    Field(r.value);
    ValueText(r, *v ? "On" : "Off");
    return changed;
}

// short lists only: each step applies at once, and a held arrow does not repeat
bool RowChoice(const char* label, int* v, const char* const* names, int n, const char* help = nullptr) {
    Row r = RowBegin(label, help);
    int step = 0;
    if (r.pressed) step = ImGui::GetIO().MousePos.x < r.value.GetCenter().x && ImGui::GetIO().MousePos.x >= r.value.Min.x ? -1 : 1;
    if (r.selected) {
        g_prompt_change = true;
        if (int d = LeftRight(false)) step = d;
    }
    if (step) *v = (*v + step + n) % n;
    Field(r.value);
    ValueText(r, names[std::clamp(*v, 0, n - 1)]);
    float pad = ImGui::GetStyle().FramePadding.x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = r.value.Min.y + (r.value.GetHeight() - ImGui::GetFontSize()) * 0.5f;
    dl->AddText(ImVec2(r.value.Min.x + pad, y), kAccent, "<");
    dl->AddText(ImVec2(r.value.Max.x - pad - ImGui::CalcTextSize(">").x, y), kAccent, ">");
    return step != 0;
}

bool RowSlider(const char* label, int* v, int lo, int hi, int step, const char* suffix = "", const char* help = nullptr) {
    Row r = RowBegin(label, help);
    bool active = ImGui::IsItemActive();
    int was = *v;
    ImGuiIO& io = ImGui::GetIO();
    if (active && ImGui::GetCurrentContext()->ActiveIdSource == ImGuiInputSource_Mouse && io.MouseClickedPos[0].x >= r.value.Min.x) {
        float t = std::clamp((io.MousePos.x - r.value.Min.x) / r.value.GetWidth(), 0.0f, 1.0f);
        *v = std::clamp(lo + int(std::lround(t * (hi - lo) / step)) * step, lo, hi);
    }
    if (r.selected) {
        g_prompt_change = true;
        if (int d = LeftRight(true, 5)) *v = std::clamp(*v + d * step, lo, hi);
    }
    Field(r.value);
    float t = hi > lo ? std::clamp(float(*v - lo) / float(hi - lo), 0.0f, 1.0f) : 0;
    ImRect bar(r.value.Min + ImVec2(1, 1), r.value.Max - ImVec2(1, 1));
    ImGui::GetWindowDrawList()->AddRectFilled(bar.Min, ImVec2(bar.Min.x + bar.GetWidth() * t, bar.Max.y), r.selected ? kSelect : kHead);
    ValueText(r, (std::to_string(*v) + suffix).c_str());
    return *v != was;
}

// a value with no set range: left and right step it, pressing the row types it
bool RowNumber(const char* label, uint32_t* v, const char* help = nullptr) {
    Row r = RowBegin(label, help);
    if (r.pressed) ImGui::OpenPopup(label);
    bool changed = false;
    if (r.selected) {
        g_prompt_change = true;
        if (int d = LeftRight(true, 10)) {
            *v = uint32_t(std::max(int64_t(*v) + d, int64_t(0)));
            changed = true;
        }
    }
    Field(r.value);
    ValueText(r, std::to_string(*v).c_str());
    ImGui::SetNextWindowPos(ImVec2(r.value.Min.x, r.value.Max.y));
    ImGui::SetNextWindowSize(ImVec2(r.value.GetWidth(), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
    if (ImGui::BeginPopup(label)) {
        int n = int(*v);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputInt("##number", &n)) {
            *v = uint32_t(std::max(n, 0));
            changed = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    return changed;
}

// a long list, or one a step at a time would be costly (a monitor, a resolution): pressing the row opens it. Between
// RowListBegin returning true and ImGui::EndPopup, one ListItem per entry
bool RowListBegin(const char* label, const char* current, const char* help = nullptr) {
    Row r = RowBegin(label, help);
    if (r.pressed) ImGui::OpenPopup(label);
    Field(r.value);
    ValueText(r, current);
    ImGui::GetWindowDrawList()->AddText(ImVec2(r.value.Max.x - ImGui::GetStyle().FramePadding.x - ImGui::CalcTextSize("v").x,
                                               r.value.Min.y + (r.value.GetHeight() - ImGui::GetFontSize()) * 0.5f),
                                        kAccent, "v");
    ImGui::SetNextWindowPos(ImVec2(r.value.Min.x, r.value.Max.y));
    ImGui::SetNextWindowSizeConstraints(ImVec2(r.value.GetWidth(), 0), ImVec2(r.value.GetWidth(), ImGui::GetFontSize() * 18));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGui::GetStyle().FramePadding);
    bool open = ImGui::BeginPopup(label);
    ImGui::PopStyleVar();
    return open;
}

bool ListItem(const char* name, bool chosen) {
    bool picked = ImGui::Selectable(name, chosen);
    if (chosen) ImGui::SetItemDefaultFocus();
    return picked;
}

enum class Bind { None, Press, Clear };

// a binding's row: press it to bind; Y, Delete, a right click or the x at its end clears it
Bind RowBind(const char* label, const std::string& value, bool waiting, const char* help = nullptr) {
    Row r = RowBegin(label, help);
    Bind out = Bind::None;
    float x = ImGui::CalcTextSize("x").x + ImGui::GetStyle().FramePadding.x * 2;
    if (r.pressed) out = ImGui::GetIO().MousePos.x >= r.value.Max.x - x && ImGui::GetIO().MouseReleased[0] ? Bind::Clear : Bind::Press;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) out = Bind::Clear;
    if (r.selected) {
        g_prompt_clear = true;
        if (ImGui::IsKeyPressed(kClearKey, false) || ImGui::IsKeyPressed(ImGuiKey_Delete, false)) out = Bind::Clear;
    }
    Field(r.value);
    ValueText(r, value.c_str());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (waiting) dl->AddRect(r.value.Min, r.value.Max, kAccent);
    dl->AddText(ImVec2(r.value.Max.x - x + ImGui::GetStyle().FramePadding.x, r.value.Min.y + (r.value.GetHeight() - ImGui::GetFontSize()) * 0.5f), kDim, "x");
    return out;
}

bool RowButton(const char* label, const char* value = nullptr, const char* help = nullptr) {
    Row r = RowBegin(label, help);
    if (value) ValueText(r, value, 1.0f);
    return r.pressed;
}

// a live reading, not selectable: the label and a bar
void RowMeter(const char* label, float fraction, const char* text) {
    float h = ImGui::GetFrameHeight();
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    float pad = ImGui::GetStyle().FramePadding.x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(pos.x + pad, pos.y + (h - ImGui::GetFontSize()) * 0.5f), kDim, label);
    ImRect bar(ImVec2(pos.x + w * 0.5f, pos.y + 2), ImVec2(pos.x + w - pad, pos.y + h - 2));
    Field(bar);
    dl->AddRectFilled(bar.Min + ImVec2(1, 1), ImVec2(bar.Min.x + 1 + (bar.GetWidth() - 2) * std::clamp(fraction, 0.0f, 1.0f), bar.Max.y - 1), kHead);
    ImVec2 size = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(bar.GetCenter().x - size.x * 0.5f, bar.GetCenter().y - size.y * 0.5f), kText, text);
}

void Section(const char* title) {
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.4f));
    float pad = ImGui::GetStyle().FramePadding.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
    if (g_bold) ImGui::PushFont(g_bold);
    ImGui::TextColored(Col(kAccent), "%s", title);
    if (g_bold) ImGui::PopFont();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(pos, ImVec2(pos.x + w, pos.y), kDark);
    dl->AddLine(ImVec2(pos.x, pos.y + 1), ImVec2(pos.x + w, pos.y + 1), kLight & 0x60ffffff);
    ImGui::Dummy(ImVec2(0, 3));
}

void Note(const char* text) {
    float pad = ImGui::GetStyle().FramePadding.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - pad);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

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
        if (RowListBegin("Monitor", current.c_str())) {
            if (ListItem("Primary display", s.monitor == 0)) {
                s.monitor = 0;
                changed = true;
            }
            for (uint32_t i = 0; i < monitors.size(); i++)
                if (ListItem(monitors[i].c_str(), s.monitor == i + 1)) {
                    s.monitor = i + 1;
                    changed = true;
                }
            ImGui::EndPopup();
        }
    }
    const char* modes[] = {"Fullscreen", "Borderless window", "Window"};
    int mode = int(s.display_mode);
    if (RowChoice("Display mode", &mode, modes, 3)) {
        s.display_mode = DisplayMode(mode);
        changed = true;
    }
    static uint32_t list_monitor = ~0u;
    static std::vector<Mode> list;
    if (list_monitor != s.monitor) list = Modes(), list_monitor = s.monitor;
    std::string current = s.width && s.height ? std::to_string(s.width) + " x " + std::to_string(s.height) : "Desktop";
    if (RowListBegin("Resolution", current.c_str())) {
        if (ListItem("Desktop", !s.width)) {
            s.width = s.height = 0;
            changed = true;
        }
        for (const Mode& m : list) {
            std::string name = std::to_string(m.width) + " x " + std::to_string(m.height);
            if (ListItem(name.c_str(), s.width == m.width && s.height == m.height)) {
                s.width = m.width;
                s.height = m.height;
                changed = true;
            }
        }
        ImGui::EndPopup();
    }
    if (RowCheck("Vertical sync", &s.vsync)) Save();
    if (changed) {
        Save();
        ApplyDisplay();
    }
    int aspect = int(s.aspect % 4);
    const char* aspects[] = {"From the screen's shape", "4:3", "16:9 (widescreen)", "Fill the screen (ultrawide)"};
    if (RowChoice("Aspect ratio", &aspect, aspects, 4, "Applies at the next start (the game reads its TV setting when it boots).")) {
        s.aspect = uint32_t(aspect);
        Save();
    }
}

void Graphics(Settings& s) {
    int percent = int(s.render_percent);
    if (RowSlider("Render scale", &percent, 25, 200, 5, "%", "Percent of the screen; applies at once. Above 100% also smooths edges.")) {
        s.render_percent = uint32_t(percent);
        Save();
    }
    const int anisos[] = {0, 2, 4, 8, 16};
    const char* aniso_names[] = {"As the game asks", "2x", "4x", "8x", "16x"};
    int a = 0;
    for (int i = 0; i < 5; i++)
        if (int(s.aniso) == anisos[i]) a = i;
    if (RowChoice("Anisotropic filtering", &a, aniso_names, 5)) {
        s.aniso = uint32_t(anisos[a]);
        Save();
    }
    const int caps[] = {30, 60, 120, 144, 240, 0};
    const char* cap_names[] = {"30", "60", "120", "144", "240", "Unlimited"};
    int c = 1;
    for (int i = 0; i < 6; i++)
        if (int(s.fps_cap) == caps[i]) c = i;
    if (RowChoice("Frame rate cap", &c, cap_names, 6, "The game sets its own pace; the cap only limits it further.")) {
        s.fps_cap = uint32_t(caps[c]);
        Save();
    }
    int ms = int(s.msaa % 4);
    const char* ms_names[] = {"Off", "2x", "4x", "8x"};
    if (RowChoice("MSAA", &ms, ms_names, 4, "Smooths the edges of the 3D scene; applies at once.")) {
        s.msaa = uint32_t(ms);
        Save();
    }
    if (RowCheck("Smooth alpha edges", &s.alpha_coverage, "With MSAA on: softens fences, leaves and other cut-out textures, which MSAA alone misses. A dark outline on some is possible.")) Save();
    int edge = s.smaa ? 2 : s.fxaa ? 1 : 0;
    const char* edge_names[] = {"Off", "FXAA", "SMAA"};
    if (RowChoice("Edge smoothing", &edge, edge_names, 3, "On the finished frame, HUD included. SMAA keeps more detail than FXAA, and costs a little more.")) {
        s.fxaa = edge == 1;
        s.smaa = edge == 2;
        Save();
    }
    int filter = int(s.scale_filter % 2);
    const char* filter_names[] = {"Bilinear", "Sharp"};
    if (RowChoice("Scaling filter", &filter, filter_names, 2, "How the frame reaches the screen's size when render scale is not 100%.")) {
        s.scale_filter = uint32_t(filter);
        Save();
    }
    if (RowCheck("Enhanced world textures", &s.world_textures, "Doubles the 3D scene's textures in the background as they appear. Uses more video memory; textures may sharpen a moment after they show. Can cause hitches of a quarter of a second as new scenery appears.")) Save();
    int sharp = int(s.sharpen);
    if (RowSlider("Sharpening", &sharp, 0, 100, 5, "%")) {
        s.sharpen = uint32_t(sharp);
        Save();
    }
    if (RowCheck("Low latency", &s.low_latency, "Applies at the next start. Holds one finished frame instead of several: less input lag, but a slow frame costs more.")) Save();
    if (RowCheck("Show frames a second", &s.show_fps)) Save();
    if (kGame.sharpen_2d && RowCheck("Sharp text and HUD", &s.sharp_2d)) Save();
    int renderer = int(s.renderer % 2);
    const char* renderers[] = {"Direct3D 12", "Direct3D 11"};
    if (RowChoice("Renderer", &renderer, renderers, 2, "Applies at the next start; Direct3D 11 for GPUs or drivers where 12 runs badly.")) {
        s.renderer = uint32_t(renderer);
        Save();
    }
}

void General(Settings& s) {
    if (RowCheck("Pause when the window is not in front", &s.pause_unfocused,
                 "The game and its sound stop while another window is in front, and carry on when you return."))
        Save();
}

void Language(Settings& s) {
    if (kGame.languages.empty()) return;
    static const char* kNames[] = {"", "English", "Japanese", "Deutsch", "Francais", "Espanol", "Italiano", "Korean", "Chinese", "Portugues"};
    auto name = [](uint32_t l) { return l < std::size(kNames) ? kNames[l] : "?"; };
    if (RowListBegin("Language", name(s.language), "Applies at the next start.")) {
        for (uint32_t l : kGame.languages)
            if (ListItem(name(l), l == s.language)) {
                s.language = l;
                Save();
            }
        ImGui::EndPopup();
    }
}

void GameOptions() {
    Language(Get());
    if (kGame.options.empty()) {
        Note("No options for this game.");
        return;
    }
    for (const Option& o : kGame.options) {
        const char* name = o.label ? o.label : o.key;
        if (o.flag) {
            if (RowCheck(name, o.flag, o.help)) Save();
        } else if (o.value) {
            if (RowNumber(name, o.value, o.help)) Save();
        }
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

// a binding's row: press it, then move or press the control; Y or the x clears it
void BindRow(const char* label, Settings::WheelBind& b, WheelDetect::Kind kind, const input::WheelInfo& w,
             const char* prompt, const char* help = nullptr) {
    bool waiting = g_detect.kind == kind && g_detect.bind == &b;
    Bind out = RowBind(label, waiting ? prompt : BindText(b), waiting, help);
    if (out == Bind::Press) g_detect = {.kind = kind, .bind = &b, .start = w.devices};
    if (out == Bind::Clear) {
        b = {};
        Save();
        input::WheelSetup();
    }
}

void Wheel(Settings& s) {
    input::WheelInfo w = input::Wheel();
    Detect(w);
    Section("Devices");
    if (w.devices.empty()) {
        Note("No wheel, pedals, shifter or button box found.");
        Note("Every DirectInput game controller is listed here (Xbox pads are under Controls).");
        Note("One plugged in later is found within two seconds.");
        g_detect = {};
        return;
    }
    for (const input::WheelDevice& d : w.devices) {
        std::string line = d.name + ": " + std::to_string(d.axes_count) + " axes, " + std::to_string(d.buttons_count) + " buttons, " +
                           std::to_string(d.povs_count) + " hats" + (d.ffb ? ", force feedback" : "") + (d.feedback ? " (in use)" : "");
        Note(line.c_str());
    }

    const char* bind_help = "Press, then turn, press or move the control on any device.";
    Section("Steering");
    float degrees = w.steer * float(s.wheel_lock) * 0.5f;
    RowMeter("Steering", 0.5f + w.steer * 0.5f, (std::to_string(int(std::lround(degrees))) + " degrees").c_str());
    BindRow("Steering axis", s.wheel_steer, WheelDetect::Steer, w, "turn the wheel to the right...", bind_help);
    if (RowCheck("Invert steering", &s.wheel_steer.invert)) Save(), input::WheelSetup();
    int range = int(s.wheel_range), lock = int(s.wheel_lock), dz = int(s.wheel_deadzone);
    if (RowSlider("Wheel rotation", &range, 180, 2880, 10, " degrees", "As set in the wheel's software."))
        s.wheel_range = uint32_t(range), Save(), input::WheelSetup();
    if (RowSlider("Steering lock", &lock, 90, 1080, 10, " degrees", "Degrees for full lock: a smaller value turns the car fully with less wheel (360 suits most cars)."))
        s.wheel_lock = uint32_t(lock), Save(), input::WheelSetup();
    if (RowSlider("Dead zone", &dz, 0, 20, 1, "%")) s.wheel_deadzone = uint32_t(dz), Save(), input::WheelSetup();

    Section("Pedals");
    struct Pedal {
        const char* name;
        Settings::WheelBind* bind;
        float value;
    } rows[3] = {{"Throttle", &s.wheel_throttle, w.throttle}, {"Brake", &s.wheel_brake, w.brake},
                 {"Clutch (unused by the game)", &s.wheel_clutch, w.clutch}};
    for (Pedal& r : rows) {
        ImGui::PushID(r.name);
        RowMeter(r.name, r.value, (std::to_string(int(std::lround(r.value * 100))) + "%").c_str());
        BindRow("Bind##pedal", *r.bind, WheelDetect::Pedal, w, "press the pedal fully...", "Let the pedal rest, press this, then press the pedal all the way.");
        ImGui::PopID();
    }

    Section("Force feedback");
    std::string on;
    for (const input::WheelDevice& d : w.devices)
        if (d.feedback) on = d.name;
    if (on.empty()) Note("None: the device the steering is bound to offers none (or another program has it).");
    else Note(("On " + on + ": " + (w.effects.empty() ? std::string("no effects made") : w.effects)).c_str());
    int strength = int(s.ffb_strength), road = int(s.ffb_road);
    const char* ffb_help = "Centring firms up with speed; kerbs, gravel and impacts shake the wheel.";
    if (RowSlider("Strength", &strength, 0, 100, 5, "%", ffb_help)) s.ffb_strength = uint32_t(strength), Save(), input::WheelSetup();
    if (RowSlider("Road and impacts", &road, 0, 100, 5, "%", ffb_help)) s.ffb_road = uint32_t(road), Save(), input::WheelSetup();
    if (RowButton("Push left")) input::WheelTestForce(-0.6f);
    if (RowButton("Push right")) input::WheelTestForce(0.6f);

    Section("Buttons");
    if (RowButton("Default buttons (Logitech G920/G923)")) {
        for (int c = 0; c < input::kControls; c++) s.wheel_buttons[c] = {.button = DefaultWheelButton(c)};
        Save();
        input::WheelSetup();
    }
    // the d-pad first (a wheel without a hat binds it to buttons), then the buttons
    for (int c = input::kUp; c <= input::kWhite; c++) {
        if (c == input::kLT || c == input::kRT) continue;
        BindRow(input::ControlLabel(c), s.wheel_buttons[c], WheelDetect::Button, w, "press a button...",
                "The steering device's hat is the d-pad too, with any d-pad buttons bound here.");
    }

    Section("H-pattern shifter");
    auto gear = [](int g) {
        return g == input::kGearUnknown ? std::string("-") : g < 0 ? std::string("R") : g == 0 ? std::string("N") : std::to_string(g);
    };
    for (int i = 0; i < kGears; i++) {
        std::string name = i == 0 ? "Reverse" : "Gear " + std::string(GearName(i));
        BindRow(name.c_str(), s.wheel_gears[i], WheelDetect::Button, w, "select the gear...");
    }
    Note(("Shifter " + gear(w.gear_selected) + ", car " + gear(w.gear_game) + ". With a manual gearbox the game's shift up and down are pressed until they match.").c_str());

    Section("Raw input");
    for (const input::WheelDevice& d : w.devices) {
        Note(d.name.c_str());
        for (int a = 0; a < 8; a++)
            if (d.axis_mask >> a & 1) RowMeter(kAxisNames[a], (d.axes[a] + 32768) / 65535.0f, std::to_string(d.axes[a]).c_str());
        std::string held;
        for (int b : d.pressed) held += (held.empty() ? "" : " ") + std::to_string(b);
        Note(("Buttons held: " + (held.empty() ? std::string("none") : held) + (d.pov >= 0 ? ", hat " + std::to_string(d.pov / 100) : "")).c_str());
    }
}

void Controls(Settings& s) {
    if (s.keys.size() != input::kControls || s.pad_map.size() != input::kControls) return;  // sized in Get
    if (RowCheck("Keyboard plays as controller 1", &s.keyboard)) Save();
    if (RowButton("Default keys")) {
        for (int c = 0; c < input::kControls; c++) s.keys[c] = DefaultKeys(c);
        Save();
    }
    if (RowButton("Default controller")) {
        for (int c = 0; c < input::kControls; c++) s.pad_map[c] = uint8_t(c);
        Save();
    }
    if (kGame.mouse_look) {
        if (RowCheck("Mouse moves the right stick", &s.mouse_look)) Save();
        if (RowCheck("Invert mouse up and down", &s.mouse_invert_y)) Save();
        int sens = int(s.mouse_sensitivity);
        if (RowSlider("Mouse sensitivity", &sens, 10, 1000, 10, "%")) {
            s.mouse_sensitivity = uint32_t(sens);
            Save();
        }
    }
    Section("Inputs");
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().CellPadding.y));
    bool table = ImGui::BeginTable("controls", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX);
    ImGui::PopStyleVar();
    if (!table) return;
    // the names and keys at their full width, the controller input takes what is left
    float pad = ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().CellPadding.x * 2;
    ImGui::TableSetupColumn("Console input", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Right stick right").x + pad);
    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("press a key...").x + pad);
    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("press a key...").x + pad);
    ImGui::TableSetupColumn("Controller input", ImGuiTableColumnFlags_WidthStretch);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    ImGui::TableHeadersRow();
    ImGui::PopItemFlag();
    const char* key_help = "Press, then press the key or mouse button (Escape clears it); Y clears it too. Each input takes up to two keys.";
    for (int c = 0; c < input::kControls; c++) {
        ImGui::PushID(c);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(input::ControlLabel(c));
        for (int slot = 0; slot < 2; slot++) {
            ImGui::TableNextColumn();
            ImGui::PushID(slot);
            bool waiting = g_bind_control == c && g_bind_slot == slot;
            std::string name = waiting ? "press a key..." : KeyToName(s.keys[c][slot]);
            if (name.empty()) name = "-";
            if (ImGui::Button((name + "###key").c_str(), ImVec2(-1, 0))) {  // the id stays as the key changes, and the selection with it
                g_bind_slot = slot;
                g_bind_control = c;
            }
            if (ImGui::IsItemFocused()) {
                g_prompt_clear = true;
                SetHelp(key_help);
                if (!waiting && (ImGui::IsKeyPressed(kClearKey, false) || ImGui::IsKeyPressed(ImGuiKey_Delete, false))) {
                    s.keys[c][slot] = 0;
                    Save();
                }
            }
            ImGui::PopID();
        }
        ImGui::TableNextColumn();
        int from = s.pad_map[c];
        if (ImGui::Button((std::string(input::ControlLabel(from)) + "###input").c_str(), ImVec2(-1, 0))) ImGui::OpenPopup("##pad");
        if (ImGui::IsItemFocused()) SetHelp("The button on your controller that presses this console input.");
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, ImGui::GetFontSize() * 18));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGui::GetStyle().FramePadding);
        bool list = ImGui::BeginPopup("##pad");
        ImGui::PopStyleVar();
        if (list) {
            for (int p = 0; p < input::kControls; p++)
                if (ListItem(input::ControlLabel(p), p == from)) {
                    s.pad_map[c] = uint8_t(p);
                    Save();
                }
            ImGui::EndPopup();
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
        uint32_t controls;
        ImGuiKey key;
    };
    constexpr auto bit = [](Control c) { return 1u << c; };
    // the left stick moves as the d-pad does. X stays out (ImGui's window switcher), and Y comes as a key ImGui gives no
    // meaning to (its own Y presses the selected item, and here Y clears a binding)
    static const Map kMap[] = {
        {bit(input::kUp) | bit(input::kLSUp), ImGuiKey_GamepadDpadUp},
        {bit(input::kDown) | bit(input::kLSDown), ImGuiKey_GamepadDpadDown},
        {bit(input::kLeft) | bit(input::kLSLeft), ImGuiKey_GamepadDpadLeft},
        {bit(input::kRight) | bit(input::kLSRight), ImGuiKey_GamepadDpadRight},
        {bit(input::kA), ImGuiKey_GamepadFaceDown},
        {bit(input::kB), ImGuiKey_GamepadFaceRight},
        {bit(input::kY), kClearKey},
        {bit(input::kWhite), ImGuiKey_GamepadL1},
        {bit(input::kBlack), ImGuiKey_GamepadR1},
    };
    static uint32_t last = 0;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    uint32_t held = g_open ? input::HeldControls() : 0;
    for (const Map& m : kMap)
        if (bool(held & m.controls) != bool(last & m.controls)) io.AddKeyEvent(m.key, held & m.controls);
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
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGui::GetStyle().FramePadding * 1.5f);
    if (ImGui::Begin("##unlock", nullptr, f)) {
        ImGui::TextColored(Col(kAccent), "Achievement unlocked");
        ImGui::Text("%s  (%u)", title.c_str(), points);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void Achievements() {
    std::vector<achievements::Status> list = achievements::List();
    uint32_t got = 0, points = 0, total = 0;
    for (auto& a : list) {
        total += a.def->points;
        if (a.unlocked) got++, points += a.def->points;
    }
    Section((std::to_string(got) + " of " + std::to_string(list.size()) + " unlocked, " + std::to_string(points) + " of " +
             std::to_string(total) + " points").c_str());
    for (auto& a : list) {
        // the rows select so a controller scrolls the list; the description is the footer's line too
        ImGui::PushID(a.def);
        Row r = RowBegin("##achievement", a.def->description);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float pad = ImGui::GetStyle().FramePadding.x, y = r.rect.Min.y + (r.rect.GetHeight() - ImGui::GetFontSize()) * 0.5f;
        dl->AddText(ImVec2(r.rect.Min.x + pad, y), r.selected ? kWhite : a.unlocked ? kAccent : kDim, a.def->title);
        std::string right = std::to_string(a.def->points) + "   " + (a.unlocked ? a.when : std::string("-"));
        dl->AddText(ImVec2(r.rect.Max.x - pad - ImGui::CalcTextSize(right.c_str()).x, y), r.text, right.c_str());
        ImGui::PopID();
    }
}

void DrawHud(double fps) {
    Scale();
    DrawUnlock();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (ScreenshotJustSaved()) {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 8), 0, ImVec2(0.5f, 0));
        ImGui::SetNextWindowBgAlpha(0.6f);
        if (ImGui::Begin("##shot", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav))
            ImGui::TextUnformatted("Screenshot saved in the screenshots folder");
        ImGui::End();
    }
    if (!Get().show_fps) return;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 8, vp->WorkPos.y + 8));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGui::GetStyle().FramePadding);
    if (ImGui::Begin("##fps", nullptr, f)) ImGui::Text("%.0f fps", fps);
    ImGui::End();
    ImGui::PopStyleVar();
}

std::vector<Mode> ListModes() { return Modes(); }

void ToggleOverlay() {
    g_open = !g_open;
    g_bind_control = -1;
    g_focus_first = g_open;
    if (!g_open) input::SwallowHeld();  // what closed it (B, Escape) stays from the game until let go
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

void OverlayInit() {
    ImGuiIO& io = ImGui::GetIO();
    // Tahoma, as classic Steam, at the size the desktop's height wants; the default font on a machine without it
    wchar_t dir[MAX_PATH];
    UINT len = GetWindowsDirectoryW(dir, MAX_PATH);
    std::filesystem::path fonts = std::filesystem::path(std::wstring(dir, len)) / L"Fonts";
    float px = std::round(kBaseFont * std::max(1.0f, float(Desktop().height) / 720.0f));
    std::error_code ec;
    if (std::filesystem::exists(fonts / L"tahoma.ttf", ec)) io.Fonts->AddFontFromFileTTF((fonts / L"tahoma.ttf").string().c_str(), px);
    if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
    if (std::filesystem::exists(fonts / L"tahomabd.ttf", ec)) g_bold = io.Fonts->AddFontFromFileTTF((fonts / L"tahomabd.ttf").string().c_str(), px);
}

namespace {

// Quit asks first: the window closes as with its own close button (Alt+F4 does the same)
void QuitPage() {
    if (RowButton("Quit game", nullptr, "Progress since the game last saved is lost.")) ImGui::OpenPopup("Quit?");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGui::GetStyle().FramePadding * 2);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImGui::GetStyle().FramePadding);
    if (ImGui::BeginPopupModal("Quit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextUnformatted("Quit the game? Progress since the game last saved is lost.");
        float w = ImGui::GetFontSize() * 7;
        if (ImGui::Button("Quit", ImVec2(w, 0))) {
            LOGR("quit from the settings window");
            Quit();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(w, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

struct Page {
    const char* name;
    void (*draw)(Settings&);
};
const std::vector<Page>& Pages() {
    static const std::vector<Page> pages = [] {
        std::vector<Page> p = {{"Display", Display}, {"Graphics", Graphics}, {"Game", [](Settings&) { GameOptions(); }}};
        if (!kGame.achievements.empty()) p.push_back({"Achievements", [](Settings&) { Achievements(); }});
        p.push_back({"General", General});
        p.push_back({"Wheel", Wheel});
        p.push_back({"Controls", Controls});
        p.push_back({"Quit", [](Settings&) { QuitPage(); }});
        return p;
    }();
    return pages;
}

std::string TitleText() {
    std::string out;
    for (const wchar_t* t = kGame.title; t && *t; t++) out += *t < 128 ? char(*t) : '?';
    return out;
}

void Prompts(float right, float y) {
    std::string text;
    if (g_prompt_change) text += "[<>] Change    ";
    if (g_prompt_clear) text += "[Y] Clear    ";
    text += "[A] Select    [LB/RB] Section    [B] Close";
    ImGui::GetWindowDrawList()->AddText(ImVec2(right - ImGui::CalcTextSize(text.c_str()).x, y), kDim, text.c_str());
}

}  // namespace

void DrawOverlay() {
    static bool busy = false;  // last frame a list was open or a binding waited: its B or Escape was for that
    if (!g_open) {
        busy = false;
        g_detect = {};
        return;
    }
    Settings& s = Get();
    Scale();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const std::vector<Page>& pages = Pages();
    static int page = 0;

    bool waiting = g_bind_control >= 0 || g_detect.kind != WheelDetect::None;
    bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (back && waiting) {
        g_bind_control = -1;
        g_detect = {};
    }
    bool close = back && !waiting && !popup && !busy;
    busy = waiting || popup;
    if (!popup && !waiting) {
        int d = (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_PageDown, false) ? 1 : 0) -
                (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_PageUp, false) ? 1 : 0);
        if (d) {
            page = (page + d + int(pages.size())) % int(pages.size());
            g_focus_first = true;
        }
    }

    // on a very wide screen the window keeps a readable shape
    float height = vp->WorkSize.y * 0.8f, width = std::min(vp->WorkSize.x * 0.6f, height * 1.5f);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(1);
    bool open = !close;
    const float em = ImGui::GetFontSize();
    if (ImGui::Begin("##settings", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                                ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 origin = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        const float pad = em * 0.6f, head_h = em * 2.1f, foot_h = em * 3.4f, side_w = em * 9.5f;

        // the header: the game's name and the close button
        dl->AddRectFilled(origin, origin + ImVec2(size.x, head_h), kHead);
        dl->AddLine(origin + ImVec2(0, head_h), origin + ImVec2(size.x, head_h), kDark);
        if (g_bold) ImGui::PushFont(g_bold);
        dl->AddText(origin + ImVec2(pad, (head_h - em) * 0.5f), kWhite, (TitleText() + " - Settings").c_str());
        if (g_bold) ImGui::PopFont();
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        ImGui::SetCursorScreenPos(origin + ImVec2(size.x - head_h, 0));
        if (ImGui::InvisibleButton("##close", ImVec2(head_h, head_h))) open = false;
        ImU32 close_col = ImGui::IsItemHovered() ? kWhite : kText;
        ImVec2 c = origin + ImVec2(size.x - head_h * 0.5f, head_h * 0.5f);
        float x = em * 0.3f;
        dl->AddLine(c - ImVec2(x, x), c + ImVec2(x, x), close_col, 1.5f);
        dl->AddLine(c + ImVec2(-x, x), c + ImVec2(x, -x), close_col, 1.5f);
        ImGui::PopItemFlag();

        // the sections
        ImGui::SetCursorScreenPos(origin + ImVec2(0, head_h + 1));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kSide);
        ImGui::BeginChild("##sections", ImVec2(side_w, size.y - head_h - 1), 0, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar);
        ImGui::Dummy(ImVec2(0, em * 0.3f));
        for (int i = 0; i < int(pages.size()); i++) {
            ImGui::PushID(i);
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImVec2 row(side_w, em * 1.9f);
            if (ImGui::InvisibleButton("##section", row) && page != i) {
                page = i;
                g_focus_first = true;
            }
            ImDrawList* side = ImGui::GetWindowDrawList();
            if (page == i) side->AddRectFilled(pos, pos + row, kSelect);
            else if (ImGui::IsItemHovered()) side->AddRectFilled(pos, pos + row, kHover);
            side->AddText(pos + ImVec2(pad, (row.y - em) * 0.5f), page == i ? kWhite : kText, pages[i].name);
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // the page; a controller's selection always stays in it
        g_help = nullptr;
        g_prompt_change = g_prompt_clear = false;
        ImGui::SetCursorScreenPos(origin + ImVec2(side_w, head_h + 1));
        ImVec2 page_size(size.x - side_w, size.y - head_h - 1 - foot_h);
        ImGui::PushID(page);  // each page's own scroll and selection
        ImGui::BeginChild("##page", page_size);
        ImGuiWindow* page_window = ImGui::GetCurrentWindow();
        if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && g.NavWindow != page_window) {
            ImGui::SetNavWindow(page_window);
            if (!page_window->NavLastIds[0]) g_focus_first = true;
            else g.NavId = page_window->NavLastIds[0];
        }
        ImGui::Dummy(ImVec2(0, em * 0.2f));
        pages[page].draw(s);
        ImGui::Dummy(ImVec2(0, em * 0.5f));
        ImGui::EndChild();
        ImGui::PopID();
        g_focus_first = false;  // a page with no rows

        // the footer: the selected row's help and the controller's buttons
        ImVec2 foot = origin + ImVec2(side_w, size.y - foot_h);
        dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(foot, origin + size, kHead);
        dl->AddLine(foot, foot + ImVec2(size.x - side_w, 0), kDark);
        if (g_help) {
            ImGui::PushFont(nullptr);
            dl->AddText(nullptr, 0, foot + ImVec2(pad, em * 0.35f), kText, g_help, nullptr, size.x - side_w - pad * 2);
            ImGui::PopFont();
        }
        Prompts(origin.x + size.x - pad, origin.y + size.y - em * 1.45f);
    }
    ImGui::End();
    if (!open) ToggleOverlay();
}

}  // namespace app
