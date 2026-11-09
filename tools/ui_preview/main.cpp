// The settings window (runtime/app/overlay.cpp) on Linux with no GPU: the real code on stubbed settings, driven through
// FeedPad as a controller drives it, drawn by a small software rasteriser. It checks the controller navigation and
// writes a PNG of each page.    ui_preview <out dir> [width height]     (exit 1 when a check fails)
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"

// --- what the overlay calls
static uint32_t g_pad;
static int g_saves, g_quits, g_swallows;
static bool g_wheel_devices = true;
static bool g_flag_a = true, g_flag_b;
static uint32_t g_value = 70;
bool GetMonitorInfoW(HMONITOR, MONITORINFOEXW*) { return true; }
bool EnumDisplaySettingsW(const wchar_t*, DWORD i, DEVMODEW* d) {
    static const DWORD m[][2] = {{3840, 2160}, {2560, 1440}, {1920, 1080}, {1280, 720}};
    if (i >= 4) return false;
    d->dmBitsPerPel = 32, d->dmPelsWidth = m[i][0], d->dmPelsHeight = m[i][1];
    return true;
}
UINT GetWindowsDirectoryW(wchar_t* out, UINT n) {  // UI_FONTS/Fonts/tahoma.ttf, when set
    const char* e = getenv("UI_WINDOWS");
    std::wstring w;
    for (const char* c = e ? e : "/nonexistent"; *c; c++) w += wchar_t(*c);
    wcsncpy(out, w.c_str(), n);
    return UINT(w.size());
}
namespace logx {
void Write(Level, const char*, ...) {}
}
namespace achievements {
static const Def kDefs[] = {{"a", "First win", "Win a race.", 10, ""}, {"b", "Kudos king", "Earn 100000 Kudos in one career.", 50, ""},
                            {"c", "World tour", "Race in every city.", 25, ""}};
std::vector<Status> List() { return {{&kDefs[0], true, "2026-10-31 20:14"}, {&kDefs[1], false, ""}, {&kDefs[2], false, ""}}; }
bool Recent(std::string&, uint32_t&, double&) { return false; }
}
namespace input {
uint32_t HeldControls() { return g_pad; }
void SwallowHeld() { g_swallows++; }
static const char* kL[kControls] = {"D-pad up", "D-pad down", "D-pad left", "D-pad right", "Start", "Back", "Left stick press", "Right stick press",
    "A", "B", "X", "Y", "Black", "White", "Left trigger", "Right trigger", "Left stick left", "Left stick right", "Left stick up", "Left stick down",
    "Right stick left", "Right stick right", "Right stick up", "Right stick down"};
const char* ControlLabel(int c) { return c >= 0 && c < kControls ? kL[c] : ""; }
const char* ControlName(int c) { return ControlLabel(c); }
void WheelSetup() {}
void WheelTestForce(float) {}
WheelInfo Wheel() {
    WheelInfo w;
    if (!g_wheel_devices) return w;
    WheelDevice d;
    d.name = "Simucube 2 Pro", d.guid = "g", d.ffb = d.feedback = true, d.axes_count = 3, d.buttons_count = 32, d.axis_mask = 7;
    d.axes[0] = 9000;
    w.devices.push_back(d);
    w.steer = 0.27f, w.throttle = 0.6f, w.effects = "spring, constant";
    return w;
}
}  // namespace input
namespace app {
static Settings* g_settings;
Settings& Get() { return *g_settings; }
void Save() { g_saves++; }
void ApplyDisplay() {}
void* Monitor() { return nullptr; }
std::vector<std::string> Monitors() { return {"Dell U4919DW", "LG C2"}; }
Mode Desktop() { return {1920, 1080}; }
void Quit() { g_quits++; }
bool ScreenshotJustSaved() { return false; }
int8_t DefaultWheelButton(int) { return 0; }
const char* GearName(int g) { static const char* n[] = {"R", "1", "2", "3", "4", "5", "6", "7"}; return n[g]; }
std::array<uint16_t, 2> DefaultKeys(int c) { return {uint16_t(c ? 'A' + c : 0), 0}; }
std::string KeyToName(uint16_t vk) { return vk ? std::string(1, char(vk)) : ""; }
const GameInfo kGame = [] {
    GameInfo g{};
    g.title = L"Project Gotham Racing 2";
    g.options = {{"skip", &g_flag_a, nullptr, "Skip intro movies", "Goes straight to the title screen."},
                 {"hud", &g_flag_b, nullptr, "Hide the HUD"},
                 {"fov", nullptr, &g_value, "Field of view", "Degrees; 70 as on the console."}};
    g.achievements = {achievements::kDefs[0], achievements::kDefs[1]};
    g.languages = {1, 3, 4};
    g.mouse_look = true;
    g.sharpen_2d = true;
    return g;
}();
}  // namespace app

#include "raster.h"

// --- the run
static Image g_image;
static std::string g_out;
static int g_failed;

static void Frame(int n = 1) {
    for (int i = 0; i < n; i++) {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1 / 60.0f;
        app::FeedPad();
        ImGui::NewFrame();
        app::DrawHud(60);
        app::DrawOverlay();
        ImGui::Render();
    }
}
static void Tap(input::Control c) {
    g_pad = 1u << c;
    Frame(2);
    g_pad = 0;
    Frame(3);
}
static void Shot(const char* name) {
    Draw(g_image);
    Png(g_image, g_out + "/" + name + ".png");
}
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); g_failed++; } } while (0)

int main(int argc, char** argv) {
    g_out = argc > 1 ? argv[1] : ".";
    int w = argc > 3 ? atoi(argv[2]) : 1920, h = argc > 3 ? atoi(argv[3]) : 1080;
    std::filesystem::create_directories(g_out);
    g_image = {w, h, std::vector<uint8_t>(size_t(w * h) * 3)};
    app::Settings s;
    s.keys.resize(input::kControls);
    s.pad_map.resize(input::kControls);
    s.wheel_buttons.resize(input::kControls);
    s.wheel_gears.resize(app::kGears);
    for (int c = 0; c < input::kControls; c++) s.keys[c] = app::DefaultKeys(c), s.pad_map[c] = uint8_t(c);
    s.render_percent = 100, s.msaa = 1, s.fxaa = false;
    app::g_settings = &s;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.DisplaySize = ImVec2(float(w), float(h));
    app::OverlayInit();
    io.Fonts->Build();

    Frame(3);
    CHECK(!app::OverlayOpen());
    app::ToggleOverlay();
    Frame(3);
    Shot("1_display");

    // a new resolution while the window is open (1280x800 from the Display page): the window is placed again inside it
    {
        auto fits = [](float sw, float sh) {
            ImGuiWindow* win = ImGui::FindWindowByName("##settings");
            return win && win->Pos.x >= 0 && win->Pos.y >= 0 && win->Pos.x + win->Size.x <= sw && win->Pos.y + win->Size.y <= sh;
        };
        auto resize = [&](int nw, int nh) {
            io.DisplaySize = ImVec2(float(nw), float(nh));
            g_image = {nw, nh, std::vector<uint8_t>(size_t(nw * nh) * 3)};
            Frame(2);
        };
        resize(1280, 800);
        CHECK(fits(1280, 800));
        Shot("1b_display_1280x800");
        resize(w, h);
        CHECK(fits(float(w), float(h)));
    }

    // down to Display mode... the first row of Display is Monitor (two monitors here): down to Vertical sync, A toggles
    bool vsync = s.vsync;
    Tap(input::kDown), Tap(input::kDown), Tap(input::kDown);
    int saves = g_saves;
    Tap(input::kA);
    CHECK(s.vsync != vsync);
    CHECK(g_saves == saves + 1);

    // a list opens with A, B closes the list and not the window
    Tap(input::kUp);
    Tap(input::kA);
    Shot("2_display_list");
    CHECK(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
    Tap(input::kB);
    CHECK(!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
    CHECK(app::OverlayOpen());

    // RB to Graphics: its first row is selected; right steps the render scale by 5
    Tap(input::kBlack);
    Tap(input::kRight);
    CHECK(s.render_percent == 105);
    Tap(input::kLeft), Tap(input::kLeft);
    CHECK(s.render_percent == 95);
    // held: repeats, and faster after a second
    g_pad = 1u << input::kRight;
    Frame(120);
    g_pad = 0;
    Frame(2);
    CHECK(s.render_percent > 130);
    s.render_percent = 150;
    // the stick moves as the d-pad does: three rows down is MSAA... scale, aniso, cap, MSAA
    for (int i = 0; i < 3; i++) Tap(input::kLSDown);
    Tap(input::kLSRight);
    CHECK(s.msaa == 2);
    Frame(2);
    Shot("3_graphics");

    Tap(input::kBlack);
    Tap(input::kDown), Tap(input::kDown);  // Language, skip intro, hide HUD
    Frame(2);
    Shot("4_game");
    bool hud = g_flag_b;
    Tap(input::kRight);
    CHECK(g_flag_b != hud);
    Tap(input::kDown);
    uint32_t fov = g_value;
    Tap(input::kRight);
    CHECK(g_value == fov + 1);

    Tap(input::kBlack);
    Frame(2);
    Shot("5_achievements");
    Tap(input::kBlack);
    Frame(2);
    Shot("6_general");

    // the wheel: Y clears a binding, B while binding only cancels it
    Tap(input::kBlack);
    Frame(2);
    Shot("7_wheel");
    CHECK(s.wheel_steer.axis == 0);
    Tap(input::kY);
    CHECK(s.wheel_steer.axis == -1);
    Tap(input::kA);
    Frame(2);
    Shot("7b_wheel_binding");
    Tap(input::kB);
    CHECK(app::OverlayOpen());

    // the controls: down the table, left and right along it, Y clears a key
    Tap(input::kBlack);
    for (int i = 0; i < 7; i++) Tap(input::kDown);  // six rows, then D-pad down's first key
    Frame(2);
    Shot("8_controls");
    CHECK(s.keys[1][0] != 0);
    Tap(input::kY);
    CHECK(s.keys[1][0] == 0);
    Tap(input::kA);  // Y did not also start a binding: A does, the window takes it and B only cancels it
    Shot("8b_controls_binding");
    CHECK(app::OverlayKey('Q'));
    CHECK(s.keys[1][0] == 'Q');
    Tap(input::kRight);
    Tap(input::kA);
    Tap(input::kB);
    CHECK(app::OverlayOpen());
    CHECK(!app::OverlayKey('Q'));

    // Quit asks first, on Cancel; B leaves the question alone without quitting or closing
    Tap(input::kBlack);
    Tap(input::kA);
    Shot("9_quit");
    Tap(input::kA);  // Cancel is selected
    CHECK(g_quits == 0);
    CHECK(!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
    Tap(input::kA);
    Tap(input::kLeft);  // to Quit
    Tap(input::kA);
    CHECK(g_quits == 1);

    // RB wraps to the first page, LB back; B closes and keeps the B from the game
    Tap(input::kBlack);
    Tap(input::kWhite);
    int swallows = g_swallows;
    Tap(input::kB);
    CHECK(!app::OverlayOpen());
    CHECK(g_swallows == swallows + 1);

    // no wheel found
    g_wheel_devices = false;
    app::ToggleOverlay();
    Frame(3);
    for (int i = 0; i < 2; i++) Tap(input::kWhite);
    Frame(2);
    Shot("7c_no_wheel");

    // the mouse and the keyboard: page keys change section, a click on a section or a row, a drag on a slider
    auto key = [&](ImGuiKey k) {
        io.AddKeyEvent(k, true);
        Frame(2);
        io.AddKeyEvent(k, false);
        Frame(3);
    };
    auto pos = [&](float x, float y) {
        io.AddMousePosEvent(x, y);
        Frame(2);
    };
    auto click = [&](float x, float y) {
        pos(x, y);
        io.AddMouseButtonEvent(0, true);
        Frame(2);
        io.AddMouseButtonEvent(0, false);
        Frame(3);
    };
    key(ImGuiKey_PageUp);  // from Wheel-2... to a known page by clicking its section
    ImGuiWindow* win = ImGui::FindWindowByName("##settings");
    CHECK(win != nullptr);
    float em = ImGui::GetFontSize(), side_x = win->Pos.x + em * 3, top = win->Pos.y + em * 2.1f + em * 0.3f + em * 1.9f * 1.5f;
    click(side_x, top);  // the second section: Graphics
    s.render_percent = 100;
    float row1 = win->Pos.y + em * 2.1f + em * 1.5f;
    float value_left = win->Pos.x + em * 9.5f + (win->Size.x - em * 9.5f) * 0.5f, right_end = win->Pos.x + win->Size.x - em;
    pos(value_left + (right_end - value_left) * 0.5f, row1);
    io.AddMouseButtonEvent(0, true);
    Frame(2);
    pos(right_end + em * 5, row1);  // dragged past the end
    CHECK(s.render_percent == 200);
    io.AddMouseButtonEvent(0, false);
    Frame(3);
    s.render_percent = 100;
    click(win->Pos.x + em * 12, row1);  // on the label: no change
    CHECK(s.render_percent == 100);
    // the keyboard: arrows work as the d-pad; after the mouse the selection is the row clicked
    key(ImGuiKey_RightArrow);
    CHECK(s.render_percent == 105);
    key(ImGuiKey_DownArrow);  // anisotropic filtering
    key(ImGuiKey_RightArrow);
    CHECK(s.aniso == 2);
    key(ImGuiKey_PageDown);
    CHECK(app::OverlayOpen());
    // Escape closes, as B; the close button too
    key(ImGuiKey_Escape);
    CHECK(!app::OverlayOpen());
    app::ToggleOverlay();
    Frame(3);
    click(win->Pos.x + win->Size.x - em, win->Pos.y + em);
    CHECK(!app::OverlayOpen());

    std::printf(g_failed ? "UI CHECKS FAILED: %d\n" : "UI CHECKS OK\n", g_failed);
    return g_failed ? 1 : 0;
}
