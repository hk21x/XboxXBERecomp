// On-screen controls for a touch screen (iOS): a stick that starts where the thumb lands, the d-pad, the face, black
// and white buttons, the triggers, Start, Back and a button for the settings window. They read as controller 1
// (input::Poll), are drawn over the frame inside the ImGui frame, and hide while a real controller is plugged in until
// the screen is touched again.
#include "app/app.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if TARGET_OS_IPHONE

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "core/input.h"

namespace app {

namespace {

constexpr int kMenu = -2;   // opens the settings window
constexpr int kStick = -3;  // the left stick

struct Button {
    int control;
    float cx, cy, hw, hh;  // centre and half sizes, in points
    bool round;
    const char* label;
    ImU32 tint;
};

struct Finger {
    int control = -1;
    float ox = 0, oy = 0, x = 0, y = 0;  // the stick's centre, and where the finger is
};

std::mutex g_lock;
SDL_Window* g_window = nullptr;
std::vector<Button> g_buttons;
float g_unit = 40, g_stick_r = 70;
float g_zone_x = 0, g_zone_y = 0;    // the stick takes a touch left of and below these
float g_rest_x = 0, g_rest_y = 0;    // where the stick is drawn when no thumb holds it
std::unordered_map<SDL_FingerID, Finger> g_fingers;
uint64_t g_last_touch = 0;

void Layout() {
    int w = 0, h = 0;
    SDL_GetWindowSize(g_window, &w, &h);
    SDL_Rect safe{0, 0, w, h};
    SDL_GetWindowSafeArea(g_window, &safe);
    const float x0 = float(safe.x) + 8, y0 = float(safe.y) + 8;
    const float x1 = float(safe.x + safe.w) - 8, y1 = float(safe.y + safe.h) - 8;
    const float u = std::min(float(h), float(w) * 0.5f) / 10.0f;
    g_unit = u;
    g_stick_r = 1.6f * u;
    g_rest_x = x0 + 2.6f * u;
    g_rest_y = y1 - 2.4f * u;
    g_zone_x = float(w) * 0.4f;
    g_zone_y = float(h) * 0.3f;

    const ImU32 white = IM_COL32(255, 255, 255, 255);
    std::vector<Button> b;
    // d-pad, top left
    const float dx = x0 + 2.4f * u, dy = y0 + 2.2f * u, ds = 0.55f * u, dstep = 1.1f * u;
    b.push_back({input::kUp, dx, dy - dstep, ds, ds, false, "", white});
    b.push_back({input::kDown, dx, dy + dstep, ds, ds, false, "", white});
    b.push_back({input::kLeft, dx - dstep, dy, ds, ds, false, "", white});
    b.push_back({input::kRight, dx + dstep, dy, ds, ds, false, "", white});
    b.push_back({kMenu, x0 + 0.7f * u, y0 + 0.5f * u, 0.6f * u, 0.4f * u, false, "MENU", white});
    // Back and Start, top centre
    b.push_back({input::kBack, float(w) / 2 - 1.3f * u, y0 + 0.5f * u, 0.8f * u, 0.4f * u, false, "BACK", white});
    b.push_back({input::kStart, float(w) / 2 + 1.3f * u, y0 + 0.5f * u, 0.8f * u, 0.4f * u, false, "START", white});
    // the triggers, bottom right: gas at the edge, brake inside it
    b.push_back({input::kRT, x1 - 1.5f * u, y1 - 2.0f * u, 1.2f * u, 1.9f * u, false, "GAS", IM_COL32(120, 255, 120, 255)});
    b.push_back({input::kLT, x1 - 4.2f * u, y1 - 1.3f * u, 1.2f * u, 1.2f * u, false, "BRAKE", IM_COL32(255, 120, 120, 255)});
    // face buttons above them
    const float fx = x1 - 3.0f * u, fy = y0 + 3.4f * u, fs = 1.15f * u, fr = 0.6f * u;
    b.push_back({input::kA, fx, fy + fs, fr, fr, true, "A", IM_COL32(110, 230, 110, 255)});
    b.push_back({input::kB, fx + fs, fy, fr, fr, true, "B", IM_COL32(240, 90, 90, 255)});
    b.push_back({input::kX, fx - fs, fy, fr, fr, true, "X", IM_COL32(100, 150, 255, 255)});
    b.push_back({input::kY, fx, fy - fs, fr, fr, true, "Y", IM_COL32(250, 220, 80, 255)});
    // white and black, either side of Y
    b.push_back({input::kWhite, fx - 2.2f * u, y0 + 0.6f * u, 0.5f * u, 0.5f * u, true, "W", IM_COL32(235, 235, 235, 255)});
    b.push_back({input::kBlack, fx + 2.2f * u, y0 + 0.6f * u, 0.5f * u, 0.5f * u, true, "BK", IM_COL32(60, 60, 60, 255)});
    g_buttons = std::move(b);
}

int Hit(float x, float y) {
    for (const Button& b : g_buttons) {
        float ex = (x - b.cx) / (b.hw * 1.15f), ey = (y - b.cy) / (b.hh * 1.15f);
        if (b.round ? ex * ex + ey * ey <= 1 : std::abs(ex) <= 1 && std::abs(ey) <= 1) return b.control;
    }
    return -1;
}

}  // namespace

void TouchInit(void* window) {
    std::lock_guard lock(g_lock);
    g_window = static_cast<SDL_Window*>(window);
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");  // the settings window takes taps as clicks
    Layout();
}

void TouchEvent(const void* event) {
    const SDL_Event& e = *static_cast<const SDL_Event*>(event);
    if (e.type == SDL_EVENT_WINDOW_RESIZED || e.type == SDL_EVENT_WINDOW_SAFE_AREA_CHANGED ||
        e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        std::lock_guard lock(g_lock);
        if (g_window) Layout();
        return;
    }
    if (e.type != SDL_EVENT_FINGER_DOWN && e.type != SDL_EVENT_FINGER_MOTION && e.type != SDL_EVENT_FINGER_UP &&
        e.type != SDL_EVENT_FINGER_CANCELED)
        return;
    bool menu = false;
    {
        std::lock_guard lock(g_lock);
        if (!g_window) return;
        int w = 0, h = 0;
        SDL_GetWindowSize(g_window, &w, &h);
        const float x = e.tfinger.x * float(w), y = e.tfinger.y * float(h);
        g_last_touch = SDL_GetTicks();
        const SDL_FingerID id = e.tfinger.fingerID;
        if (e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED) {
            g_fingers.erase(id);
            return;
        }
        if (e.type == SDL_EVENT_FINGER_DOWN) {
            Finger f;
            f.x = x, f.y = y;
            f.control = Hit(x, y);
            if (f.control == kMenu) {
                menu = true;
                f.control = -1;
            } else if (f.control == -1 && x < g_zone_x && y > g_zone_y) {
                f.control = kStick;
                f.ox = x, f.oy = y;
            }
            if (OverlayOpen() && f.control != -1) f.control = -1;  // the window takes the taps
            g_fingers[id] = f;
        } else if (auto it = g_fingers.find(id); it != g_fingers.end()) {
            Finger& f = it->second;
            f.x = x, f.y = y;
            if (f.control == kStick) {
                // the centre follows a thumb that goes past the rim, so turning back answers at once
                float ddx = x - f.ox, ddy = y - f.oy, d = std::sqrt(ddx * ddx + ddy * ddy);
                if (d > g_stick_r) f.ox = x - ddx / d * g_stick_r, f.oy = y - ddy / d * g_stick_r;
            } else if (f.control != -1 || !OverlayOpen()) {
                // a thumb slides between buttons (gas to brake)
                int c = Hit(x, y);
                if (c >= 0) f.control = c;
            }
        }
    }
    if (menu) {
        std::lock_guard lock(UiLock());
        ToggleOverlay();
    }
}

bool TouchRead(float* v) {
    std::lock_guard lock(g_lock);
    if (!g_window || OverlayOpen()) return true;
    for (auto& [id, f] : g_fingers) {
        if (f.control >= 0 && f.control < input::kControls) v[f.control] = 1.0f;
        if (f.control == kStick) {
            float sx = std::clamp((f.x - f.ox) / g_stick_r, -1.0f, 1.0f);
            float sy = std::clamp((f.y - f.oy) / g_stick_r, -1.0f, 1.0f);
            v[input::kLSLeft] = std::max(v[input::kLSLeft], -sx), v[input::kLSRight] = std::max(v[input::kLSRight], sx);
            v[input::kLSUp] = std::max(v[input::kLSUp], -sy), v[input::kLSDown] = std::max(v[input::kLSDown], sy);
        }
    }
    return true;
}

void DrawTouch() {
    std::lock_guard lock(g_lock);
    if (!g_window) return;
    // a real controller hides them until the screen is touched
    if (SDL_HasGamepad() && g_fingers.empty() && SDL_GetTicks() - g_last_touch > 8000) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const bool open = OverlayOpen();
    bool held[input::kControls + 1] = {};
    const Finger* stick = nullptr;
    for (auto& [id, f] : g_fingers) {
        if (f.control >= 0 && f.control < input::kControls) held[f.control] = true;
        if (f.control == kStick) stick = &f;
    }
    ImFont* font = ImGui::GetFont();
    const float text = 0.42f * g_unit;
    auto label = [&](const char* s, float cx, float cy, ImU32 col) {
        if (!*s) return;
        ImVec2 size = font->CalcTextSizeA(text, FLT_MAX, 0, s);
        dl->AddText(font, text, ImVec2(cx - size.x / 2, cy - size.y / 2), col, s);
    };
    for (const Button& b : g_buttons) {
        if (open && b.control != kMenu) continue;
        const bool on = b.control >= 0 && held[b.control];
        const ImU32 fill = (b.tint & 0x00FFFFFF) | (on ? 0x90000000u : 0x38000000u);
        const ImU32 edge = IM_COL32(255, 255, 255, on ? 200 : 110);
        if (b.round) {
            dl->AddCircleFilled(ImVec2(b.cx, b.cy), b.hw, fill, 32);
            dl->AddCircle(ImVec2(b.cx, b.cy), b.hw, edge, 32, 2.0f);
        } else {
            ImVec2 a(b.cx - b.hw, b.cy - b.hh), z(b.cx + b.hw, b.cy + b.hh);
            float r = std::min(b.hw, b.hh) * 0.35f;
            dl->AddRectFilled(a, z, fill, r);
            dl->AddRect(a, z, edge, r, 0, 2.0f);
        }
        label(b.label, b.cx, b.cy, IM_COL32(255, 255, 255, on ? 255 : 190));
        // the d-pad's arrows
        const float t = 0.25f * g_unit;
        const ImU32 arrow = IM_COL32(255, 255, 255, on ? 255 : 170);
        if (b.control == input::kUp)
            dl->AddTriangleFilled(ImVec2(b.cx, b.cy - t), ImVec2(b.cx - t, b.cy + t), ImVec2(b.cx + t, b.cy + t), arrow);
        if (b.control == input::kDown)
            dl->AddTriangleFilled(ImVec2(b.cx, b.cy + t), ImVec2(b.cx + t, b.cy - t), ImVec2(b.cx - t, b.cy - t), arrow);
        if (b.control == input::kLeft)
            dl->AddTriangleFilled(ImVec2(b.cx - t, b.cy), ImVec2(b.cx + t, b.cy + t), ImVec2(b.cx + t, b.cy - t), arrow);
        if (b.control == input::kRight)
            dl->AddTriangleFilled(ImVec2(b.cx + t, b.cy), ImVec2(b.cx - t, b.cy - t), ImVec2(b.cx - t, b.cy + t), arrow);
    }
    if (open) return;
    // the stick: its ring where the thumb landed, the knob under the thumb
    float cx = stick ? stick->ox : g_rest_x, cy = stick ? stick->oy : g_rest_y;
    float kx = cx, ky = cy;
    if (stick) {
        float ddx = stick->x - cx, ddy = stick->y - cy, d = std::sqrt(ddx * ddx + ddy * ddy);
        float k = d > g_stick_r ? g_stick_r / d : 1.0f;
        kx = cx + ddx * k, ky = cy + ddy * k;
    }
    dl->AddCircleFilled(ImVec2(cx, cy), g_stick_r, IM_COL32(255, 255, 255, stick ? 50 : 28), 48);
    dl->AddCircle(ImVec2(cx, cy), g_stick_r, IM_COL32(255, 255, 255, stick ? 170 : 100), 48, 2.0f);
    dl->AddCircleFilled(ImVec2(kx, ky), 0.7f * g_unit, IM_COL32(255, 255, 255, stick ? 140 : 70), 32);
}

}  // namespace app

#else

namespace app {
void TouchInit(void*) {}
void TouchEvent(const void*) {}
bool TouchRead(float*) { return false; }
void DrawTouch() {}
}  // namespace app

#endif
