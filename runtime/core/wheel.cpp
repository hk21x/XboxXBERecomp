// A steering wheel through DirectInput: its axes, buttons and force feedback (spring, damper, constant force, sine).
// DirectInput rather than SDL: SDL's DirectInput force feedback needs its DirectInput joysticks on, which would take the
// Xbox pads from Windows.Gaming.Input (and merge their triggers). Only driving devices are opened here, so the pads
// stay with SDL. Logitech's G920, G923 and G29 report as driving devices with force feedback once G HUB is installed.
#include "core/input.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "app/app.h"
#include "core/log.h"

namespace input {

namespace {

using Clock = std::chrono::steady_clock;

constexpr LONG kForceMax = DI_FFNOMINALMAX;  // 10000

struct Effect {
    IDirectInputEffect* fx = nullptr;
    LONG last = -1;  // the strength last set, to send only changes
};

struct Device {
    IDirectInput8W* di = nullptr;
    IDirectInputDevice8W* dev = nullptr;
    HWND hwnd = nullptr;
    std::string name;
    int buttons = 0;
    bool ffb = false;
    std::string effects;
    Effect spring, damper, constant, sine;
    DIJOYSTATE2 state{};
    bool have_state = false;
    Clock::time_point next_scan{}, next_ffb{};
} g;  // the wheel thread's own
std::mutex g_wheel_lock;

std::string Narrow(const wchar_t* w) {
    std::string s;
    for (; *w; w++) s += *w < 128 ? char(*w) : '?';
    return s;
}

// a driving device, or a Logitech wheel listed as another kind
bool IsWheel(const DIDEVICEINSTANCEW& d) {
    if (GET_DIDEVICE_TYPE(d.dwDevType) == DI8DEVTYPE_DRIVING) return true;
    std::string n = Narrow(d.tszProductName);
    for (char& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
    return n.find("wheel") != std::string::npos || n.find("racing") != std::string::npos;
}

BOOL CALLBACK FindWheel(const DIDEVICEINSTANCEW* d, void* out) {
    if (!IsWheel(*d)) return DIENUM_CONTINUE;
    *static_cast<DIDEVICEINSTANCEW*>(out) = *d;
    return DIENUM_STOP;
}

void SetRange(IDirectInputDevice8W* dev) {
    DIPROPRANGE r{};
    r.diph.dwSize = sizeof(r);
    r.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    r.diph.dwHow = DIPH_DEVICE;
    r.lMin = -32768;
    r.lMax = 32767;
    dev->SetProperty(DIPROP_RANGE, &r.diph);
    DIPROPDWORD z{};
    z.diph.dwSize = sizeof(z);
    z.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    z.diph.dwHow = DIPH_DEVICE;
    z.dwData = 0;
    dev->SetProperty(DIPROP_DEADZONE, &z.diph);
    // the wheel's own centring spring off: the force feedback below makes one
    z.dwData = DIPROPAUTOCENTER_OFF;
    dev->SetProperty(DIPROP_AUTOCENTER, &z.diph);
}

// an effect on the steering axis (X), made stopped; the type specific parameters are set as it runs
bool MakeEffect(const GUID& guid, Effect& e, void* params, DWORD params_size, const char* name) {
    DWORD axes[1] = {DIJOFS_X};
    LONG direction[1] = {0};
    DIEFFECT eff{};
    eff.dwSize = sizeof(eff);
    eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    eff.dwDuration = INFINITE;
    eff.dwGain = DI_FFNOMINALMAX;
    eff.dwTriggerButton = DIEB_NOTRIGGER;
    eff.cAxes = 1;
    eff.rgdwAxes = axes;
    eff.rglDirection = direction;
    eff.cbTypeSpecificParams = params_size;
    eff.lpvTypeSpecificParams = params;
    if (FAILED(g.dev->CreateEffect(guid, &eff, &e.fx, nullptr)) || !e.fx) {
        e.fx = nullptr;
        return false;
    }
    e.fx->Start(1, 0);
    if (!g.effects.empty()) g.effects += ", ";
    g.effects += name;
    return true;
}

void MakeEffects() {
    DICONDITION cond{};
    MakeEffect(GUID_Spring, g.spring, &cond, sizeof(cond), "spring");
    MakeEffect(GUID_Damper, g.damper, &cond, sizeof(cond), "damper");
    DICONSTANTFORCE constant{};
    MakeEffect(GUID_ConstantForce, g.constant, &constant, sizeof(constant), "constant");
    DIPERIODIC periodic{};
    periodic.dwPeriod = 40000;  // microseconds
    MakeEffect(GUID_Sine, g.sine, &periodic, sizeof(periodic), "sine");
    g.ffb = g.spring.fx || g.damper.fx || g.constant.fx || g.sine.fx;
}

void ReleaseEffects() {
    for (Effect* e : {&g.spring, &g.damper, &g.constant, &g.sine}) {
        if (e->fx) {
            e->fx->Stop();
            e->fx->Release();
        }
        *e = {};
    }
    g.effects.clear();
    g.ffb = false;
}

void Close() {
    ReleaseEffects();
    if (g.dev) {
        g.dev->Unacquire();
        g.dev->Release();
        g.dev = nullptr;
        LOGR("wheel: %s unplugged", g.name.c_str());
    }
    g.have_state = false;
}

// a wheel looked for every two seconds while there is none
void Scan() {
    auto now = Clock::now();
    if (g.dev || !g.di || now < g.next_scan) return;
    g.next_scan = now + std::chrono::seconds(2);
    DIDEVICEINSTANCEW found{};
    if (FAILED(g.di->EnumDevices(DI8DEVCLASS_GAMECTRL, FindWheel, &found, DIEDFL_ATTACHEDONLY)) || !found.dwSize) return;
    IDirectInputDevice8W* dev = nullptr;
    if (FAILED(g.di->CreateDevice(found.guidInstance, &dev, nullptr))) return;
    if (FAILED(dev->SetDataFormat(&c_dfDIJoystick2))) {
        dev->Release();
        return;
    }
    // exclusive is what force feedback needs; in front only, as the game's input is
    bool exclusive = SUCCEEDED(dev->SetCooperativeLevel(g.hwnd, DISCL_EXCLUSIVE | DISCL_FOREGROUND));
    if (!exclusive) dev->SetCooperativeLevel(g.hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    SetRange(dev);
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    dev->GetCapabilities(&caps);
    g.dev = dev;
    g.name = Narrow(found.tszProductName);
    g.buttons = int(caps.dwButtons);
    uint32_t vidpid = found.guidProduct.Data1;
    dev->Acquire();
    if (exclusive && (caps.dwFlags & DIDC_FORCEFEEDBACK)) MakeEffects();
    LOGR("wheel: %s (USB %04X:%04X), %lu axes, %lu buttons, %lu hats, force feedback %s%s%s", g.name.c_str(),
         vidpid & 0xFFFF, vidpid >> 16, caps.dwAxes, caps.dwButtons, caps.dwPOVs,
         (caps.dwFlags & DIDC_FORCEFEEDBACK) ? (g.ffb ? "on: " : "made no effects") : "not offered",
         g.effects.c_str(), exclusive ? "" : " (not exclusive: no force feedback)");
}

// the state, acquiring again after the window lost the focus; false when the wheel is gone
bool Read() {
    if (!g.dev) return false;
    g.dev->Poll();
    HRESULT hr = g.dev->GetDeviceState(sizeof(g.state), &g.state);
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        if (SUCCEEDED(g.dev->Acquire())) {
            g.dev->Poll();
            hr = g.dev->GetDeviceState(sizeof(g.state), &g.state);
            // effects stop when the device is lost: started again
            for (Effect* e : {&g.spring, &g.damper, &g.constant, &g.sine})
                if (e->fx) e->fx->Start(1, 0), e->last = -1;
        }
    }
    if (hr == DIERR_UNPLUGGED) {
        Close();
        return false;
    }
    g.have_state = SUCCEEDED(hr);
    return g.have_state;
}

int32_t Axis(const DIJOYSTATE2& st, int i) {
    switch (i) {
        case 0: return st.lX;
        case 1: return st.lY;
        case 2: return st.lZ;
        case 3: return st.lRx;
        case 4: return st.lRy;
        case 5: return st.lRz;
        case 6: return st.rglSlider[0];
        case 7: return st.rglSlider[1];
        default: return 0;
    }
}

float PedalValue(const DIJOYSTATE2& st, const app::Settings::Pedal& p) {
    if (p.axis < 0 || p.axis > 7 || p.full == p.rest) return 0;
    return std::clamp(float(Axis(st, p.axis) - p.rest) / float(p.full - p.rest), 0.0f, 1.0f);
}

// -1 to 1 for the game: the wheel's angle against the steering lock, past the dead zone
float SteerValue(const DIJOYSTATE2& st, const app::Settings& s) {
    if (s.wheel_steer_axis < 0 || s.wheel_steer_axis > 7) return 0;
    float v = std::clamp(float(Axis(st, s.wheel_steer_axis)) / 32767.0f, -1.0f, 1.0f);
    if (s.wheel_steer_invert) v = -v;
    float degrees = v * float(s.wheel_range) * 0.5f;
    float out = degrees / std::max(float(s.wheel_lock) * 0.5f, 1.0f);
    float dz = float(s.wheel_deadzone) / 100.0f;
    if (dz > 0) out = std::fabs(out) <= dz ? 0 : (out - std::copysign(dz, out)) / (1 - dz);
    return std::clamp(out, -1.0f, 1.0f);
}

void SetCondition(Effect& e, LONG coefficient) {
    if (!e.fx || coefficient == e.last) return;
    e.last = coefficient;
    DICONDITION c{};
    c.lPositiveCoefficient = c.lNegativeCoefficient = coefficient;
    c.dwPositiveSaturation = c.dwNegativeSaturation = DWORD(kForceMax);
    DIEFFECT eff{};
    eff.dwSize = sizeof(eff);
    eff.cbTypeSpecificParams = sizeof(c);
    eff.lpvTypeSpecificParams = &c;
    e.fx->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS);
}

void SetConstant(LONG level) {
    Effect& e = g.constant;
    if (!e.fx || level == e.last) return;
    e.last = level;
    DICONSTANTFORCE c{level};
    DIEFFECT eff{};
    eff.dwSize = sizeof(eff);
    eff.cbTypeSpecificParams = sizeof(c);
    eff.lpvTypeSpecificParams = &c;
    e.fx->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS);
}

void SetSine(LONG magnitude, DWORD period_us) {
    Effect& e = g.sine;
    LONG key = magnitude * 1000 + LONG(period_us / 1000);
    if (!e.fx || key == e.last) return;
    e.last = key;
    DIPERIODIC p{};
    p.dwMagnitude = DWORD(magnitude);
    p.dwPeriod = period_us;
    DIEFFECT eff{};
    eff.dwSize = sizeof(eff);
    eff.cbTypeSpecificParams = sizeof(p);
    eff.lpvTypeSpecificParams = &p;
    e.fx->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS);
}

// What the game's threads and the wheel thread share (g_wheel_lock): the last state read, the forces asked for. Every
// DirectInput call (looking for a wheel, reading it, setting its forces over USB) is on the wheel thread, never on a
// thread the game waits on: looking for devices can take a tenth of a second.
struct Shared {
    bool connected = false, ffb = false;
    std::string name, effects;
    int buttons = 0;
    DIJOYSTATE2 state{};
    WheelFeel feel;
    uint16_t rumble_left = 0, rumble_right = 0;
    bool feedback = false;  // the game has asked for forces since the wheel was found
    Clock::time_point test_until{};
    float test_level = 0;
} s_shared;

// Forces, at most 60 times a second:
// - spring: centring, light in the menus, firmer with speed while racing (a car's self-aligning feel)
// - damper: a steady weight, so the wheel does not oscillate
// - sine: the game's rumble (kerbs, gravel, impacts), its low motor as a slow shake, its high one as a fine buzz
// - constant: the settings window's test pulse
void Feedback(const Shared& want, Clock::time_point now) {
    if (!g.ffb || now < g.next_ffb) return;
    g.next_ffb = now + std::chrono::milliseconds(16);
    const app::Settings& s = app::Get();
    const WheelFeel& feel = want.feel;
    float strength = std::clamp(float(s.ffb_strength) / 100.0f, 0.0f, 1.0f);
    bool paused = app::OverlayOpen() && now >= want.test_until;
    float speed = std::clamp(std::fabs(feel.speed) / 40.0f, 0.0f, 1.0f);  // full by 40 m/s, about 90 mph
    float spring = feel.racing ? 0.15f + 0.65f * speed : 0.25f;
    float damper = feel.racing ? 0.12f + 0.10f * speed : 0.10f;
    if (paused) spring = 0.25f, damper = 0.10f;
    SetCondition(g.spring, LONG(spring * strength * kForceMax));
    SetCondition(g.damper, LONG(damper * strength * kForceMax));
    float road = std::clamp(float(s.ffb_road) / 100.0f, 0.0f, 1.0f);
    float low = want.rumble_left / 65535.0f, high = want.rumble_right / 65535.0f;
    float shake = feel.racing && !paused ? std::max(low, high) * road * strength : 0;
    SetSine(LONG(shake * 0.6f * kForceMax), low >= high ? 60000 : 25000);
    float test = now < want.test_until ? want.test_level : 0;
    SetConstant(LONG(std::clamp(test, -1.0f, 1.0f) * strength * kForceMax));
}

// the wheel thread: looks for a wheel every two seconds while there is none, reads it every 2 ms, sets its forces
void Run() {
    if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                  reinterpret_cast<void**>(&g.di), nullptr))) {
        LOGW("wheel: DirectInput unavailable, no steering wheels");
        g.di = nullptr;
        return;
    }
    Scan();
    if (!g.dev) LOGR("wheel: none found (one plugged in later is picked up)");
    for (;;) {
        Scan();
        bool ok = Read();
        Shared want;
        {
            std::lock_guard lock(g_wheel_lock);
            s_shared.connected = ok;
            s_shared.ffb = g.ffb;
            s_shared.name = g.name;
            s_shared.effects = g.effects;
            s_shared.buttons = g.buttons;
            if (ok) s_shared.state = g.state;
            if (!g.dev) s_shared.feedback = false;
            want = s_shared;
        }
        if (ok && want.feedback) Feedback(want, Clock::now());
        Sleep(2);
    }
}

}  // namespace

void WheelInit(void* hwnd) {
    g.hwnd = static_cast<HWND>(hwnd);
    std::thread(Run).detach();
}

bool WheelRead(float* v) {
    DIJOYSTATE2 st;
    {
        std::lock_guard lock(g_wheel_lock);
        if (!s_shared.connected) return false;
        st = s_shared.state;
    }
    const app::Settings& s = app::Get();
    float steer = SteerValue(st, s);
    v[kLSLeft] = std::max(-steer, 0.0f);
    v[kLSRight] = std::max(steer, 0.0f);
    v[kRT] = PedalValue(st, s.wheel_throttle);
    v[kLT] = PedalValue(st, s.wheel_brake);
    for (int c = 0; c < kControls; c++) {
        int b = c < int(s.wheel_buttons.size()) ? s.wheel_buttons[c] : -1;
        if (b >= 0 && b < 128 && (st.rgbButtons[b] & 0x80)) v[c] = 1.0f;
    }
    // the hat as the d-pad, diagonals pressing both
    DWORD pov = st.rgdwPOV[0];
    if (LOWORD(pov) != 0xFFFF) {
        int deg = int(pov / 100) % 360;
        if (deg >= 315 || deg <= 45) v[kUp] = 1;
        if (deg >= 45 && deg <= 135) v[kRight] = 1;
        if (deg >= 135 && deg <= 225) v[kDown] = 1;
        if (deg >= 225 && deg <= 315) v[kLeft] = 1;
    }
    return true;
}

void WheelFeedback(const WheelFeel& feel, uint16_t rumble_left, uint16_t rumble_right) {
    std::lock_guard lock(g_wheel_lock);
    s_shared.feel = feel;
    s_shared.rumble_left = rumble_left;
    s_shared.rumble_right = rumble_right;
    s_shared.feedback = true;
}

WheelInfo Wheel() {
    DIJOYSTATE2 st;
    WheelInfo w;
    {
        std::lock_guard lock(g_wheel_lock);
        w.connected = s_shared.connected;
        w.name = s_shared.name;
        w.ffb = s_shared.ffb;
        w.effects = s_shared.effects;
        w.buttons_count = s_shared.buttons;
        st = s_shared.state;
    }
    if (!w.connected) return w;
    for (int i = 0; i < 8; i++) w.axes[i] = Axis(st, i);
    for (int b = 0; b < 128; b++)
        if (st.rgbButtons[b] & 0x80) w.pressed.push_back(b);
    if (LOWORD(st.rgdwPOV[0]) != 0xFFFF) w.pov = int(st.rgdwPOV[0]);
    const app::Settings& s = app::Get();
    w.steer = SteerValue(st, s);
    w.throttle = PedalValue(st, s.wheel_throttle);
    w.brake = PedalValue(st, s.wheel_brake);
    w.clutch = PedalValue(st, s.wheel_clutch);
    return w;
}

void WheelTestForce(float level) {
    std::lock_guard lock(g_wheel_lock);
    s_shared.test_level = level;
    s_shared.test_until = Clock::now() + std::chrono::milliseconds(500);
    s_shared.feedback = true;
}

}  // namespace input
