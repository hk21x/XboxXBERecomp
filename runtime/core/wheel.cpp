// A steering wheel and the rest of a rig through DirectInput: every game controller DirectInput lists (wheel bases,
// pedal boxes, button boxes, shifters, as separate devices or one) except Xbox pads, which stay with SDL. Each console
// input is bound to an axis or a button on one of them (app::Settings::WheelBind, set in the settings window by moving
// or pressing it); the forces go to the device the steering is bound to, on the steering's axis.
// DirectInput rather than SDL: SDL's DirectInput force feedback needs its DirectInput joysticks on, which would take the
// Xbox pads from Windows.Gaming.Input (and merge their triggers).
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
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "core/log.h"

namespace input {

namespace {

using Clock = std::chrono::steady_clock;
using Bind = app::Settings::WheelBind;

constexpr LONG kForceMax = DI_FFNOMINALMAX;  // 10000
constexpr DWORD kAxisOffsets[8] = {DIJOFS_X,  DIJOFS_Y,  DIJOFS_Z,         DIJOFS_RX,
                                   DIJOFS_RY, DIJOFS_RZ, DIJOFS_SLIDER(0), DIJOFS_SLIDER(1)};

std::string Narrow(const wchar_t* w) {
    std::string s;
    for (; *w; w++) s += *w < 128 ? char(*w) : '?';
    return s;
}

std::string GuidText(const GUID& g) {
    char s[40];
    std::snprintf(s, sizeof(s), "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g.Data1, g.Data2, g.Data3,
                  g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return s;
}

// the bindings, copied from the settings (WheelSetup): the game's threads never read the settings' strings
struct Config {
    Bind steer, throttle, brake, clutch;
    Bind buttons[kControls];
    Bind gears[app::kGears];
    uint32_t range = 900, lock = 360, deadzone = 0, ffb_strength = 70, ffb_road = 50;
};

// a device as the game's threads see it (the wheel thread's last read)
struct View {
    std::string name, guid;
    DWORD type = 0;
    bool ffb = false, ok = false;
    int axes = 0, buttons = 0, povs = 0;
    uint8_t axis_mask = 0;  // which of DIJOYSTATE2's eight axes the device has (Axis's order)
    DIJOYSTATE2 state{};
};

struct Effect {
    IDirectInputEffect* fx = nullptr;
    LONG last = -1;  // the strength last set, to send only changes
};

// the wheel thread's own: the devices open, the one with the effects
struct Device {
    IDirectInputDevice8W* dev = nullptr;
    View view;
    bool exclusive = false;
    int failures = 0;
};
struct Rig {
    IDirectInput8W* di = nullptr;
    HWND hwnd = nullptr;
    std::vector<Device> devs;
    std::set<std::string> skipped;  // Xbox pads, left to SDL (said once)
    int ffb_dev = -1, ffb_axis = -1;  // where the effects are
    std::string effects;
    Effect spring, damper, constant, sine;
    Clock::time_point next_scan{}, next_ffb{};
} g;

// What the game's threads and the wheel thread share (g_lock): the devices' last state, the bindings, the forces
// asked for. Every DirectInput call (looking for devices, reading them, setting forces over USB) is on the wheel
// thread, never on a thread the game waits on: looking for devices can take a tenth of a second.
struct Shared {
    std::vector<View> devs;
    Config config;
    int ffb_dev = -1;
    std::string effects;
    WheelFeel feel;
    uint16_t rumble_left = 0, rumble_right = 0;
    bool feedback = false;  // the game has asked for forces since the device was set up
    Clock::time_point test_until{};
    float test_level = 0;
    // the H-pattern shifter: the console shift input pressed until, and the next press allowed from
    int shift_control = -1;
    Clock::time_point shift_until{}, shift_next{};
} s;
std::mutex g_lock;

// the device a binding reads, -1 none: no device named means the rig's wheel found by itself (the first driving
// device; a rumble pad that is not an Xbox pad would read its centred sticks as half pedals, so anything else is
// bound in the Wheel tab); a name with a GUID prefers that instance, then any of that name
int AutoDevice(const std::vector<View>& devs) {
    for (size_t i = 0; i < devs.size(); i++)
        if (GET_DIDEVICE_TYPE(devs[i].type) == DI8DEVTYPE_DRIVING) return int(i);
    return -1;
}
int FindDevice(const std::vector<View>& devs, const Bind& b) {
    if (b.axis < 0 && b.button < 0) return -1;
    if (b.device.empty()) return AutoDevice(devs);
    for (size_t i = 0; i < devs.size(); i++)
        if (devs[i].name == b.device && !b.guid.empty() && devs[i].guid == b.guid) return int(i);
    for (size_t i = 0; i < devs.size(); i++)
        if (devs[i].name == b.device) return int(i);
    return -1;
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

// a binding's device state, null when it is not there
const DIJOYSTATE2* StateOf(const std::vector<View>& devs, const Bind& b) {
    int d = FindDevice(devs, b);
    return d >= 0 && devs[size_t(d)].ok ? &devs[size_t(d)].state : nullptr;
}

float PedalValue(const std::vector<View>& devs, const Bind& p) {
    const DIJOYSTATE2* st = StateOf(devs, p);
    if (!st || p.axis < 0 || p.axis > 7 || p.full == p.rest) return 0;
    return std::clamp(float(Axis(*st, p.axis) - p.rest) / float(p.full - p.rest), 0.0f, 1.0f);
}

// -1 to 1 for the game: the wheel's angle against the steering lock, past the dead zone
float SteerValue(const std::vector<View>& devs, const Config& c) {
    const DIJOYSTATE2* st = StateOf(devs, c.steer);
    if (!st || c.steer.axis < 0 || c.steer.axis > 7) return 0;
    float v = std::clamp(float(Axis(*st, c.steer.axis)) / 32767.0f, -1.0f, 1.0f);
    if (c.steer.invert) v = -v;
    float degrees = v * float(c.range) * 0.5f;
    float out = degrees / std::max(float(c.lock) * 0.5f, 1.0f);
    float dz = float(c.deadzone) / 100.0f;
    if (dz > 0) out = std::fabs(out) <= dz ? 0 : (out - std::copysign(dz, out)) / (1 - dz);
    return std::clamp(out, -1.0f, 1.0f);
}

bool Pressed(const std::vector<View>& devs, const Bind& b) {
    const DIJOYSTATE2* st = StateOf(devs, b);
    return st && b.button >= 0 && b.button < 128 && (st->rgbButtons[b.button] & 0x80);
}

// the shifter's position: -1 reverse, 1 to 7, kGearUnknown in neutral or with no shifter bound
int SelectedGear(const std::vector<View>& devs, const Config& c) {
    for (int i = 0; i < app::kGears; i++)
        if (Pressed(devs, c.gears[i])) return i == 0 ? -1 : i;
    return kGearUnknown;
}

// ---- the wheel thread ----

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
}

// an Xbox pad (XInput): its device path names "IG_"
bool IsXInput(IDirectInputDevice8W* dev) {
    DIPROPGUIDANDPATH p{};
    p.diph.dwSize = sizeof(p);
    p.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    p.diph.dwHow = DIPH_DEVICE;
    if (FAILED(dev->GetProperty(DIPROP_GUIDANDPATH, &p.diph))) return false;
    std::wstring path = p.wszPath;
    for (wchar_t& c : path) c = wchar_t(std::towupper(c));
    return path.find(L"IG_") != std::wstring::npos;
}

// the axes a device has, by type: X Y Z Rx Ry Rz, then its sliders in order (DIJOYSTATE2's two)
struct AxisScan {
    uint8_t mask = 0;
    int sliders = 0;
};
BOOL CALLBACK CollectAxis(const DIDEVICEOBJECTINSTANCEW* o, void* out) {
    auto* scan = static_cast<AxisScan*>(out);
    const GUID* kTypes[6] = {&GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis, &GUID_RyAxis, &GUID_RzAxis};
    for (int i = 0; i < 6; i++)
        if (IsEqualGUID(o->guidType, *kTypes[i])) scan->mask |= uint8_t(1u << i);
    if (IsEqualGUID(o->guidType, GUID_Slider) && scan->sliders < 2) scan->mask |= uint8_t(1u << (6 + scan->sliders++));
    return DIENUM_CONTINUE;
}

BOOL CALLBACK Collect(const DIDEVICEINSTANCEW* d, void* out) {
    static_cast<std::vector<DIDEVICEINSTANCEW>*>(out)->push_back(*d);
    return DIENUM_CONTINUE;
}

const char* TypeName(DWORD type) {
    switch (GET_DIDEVICE_TYPE(type)) {
        case DI8DEVTYPE_DRIVING: return "driving";
        case DI8DEVTYPE_JOYSTICK: return "joystick";
        case DI8DEVTYPE_GAMEPAD: return "gamepad";
        case DI8DEVTYPE_FLIGHT: return "flight";
        case DI8DEVTYPE_1STPERSON: return "first person";
        case DI8DEVTYPE_SUPPLEMENTAL: return "supplemental";
        default: return "other";
    }
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
}

// read by anyone, in the background: the device with the effects is in front and exclusive instead (force feedback
// needs that). Changing it means letting go of the device first.
bool SetMode(Device& d, bool exclusive) {
    d.dev->Unacquire();
    bool ok = SUCCEEDED(d.dev->SetCooperativeLevel(g.hwnd, exclusive ? DISCL_EXCLUSIVE | DISCL_FOREGROUND
                                                                     : DISCL_NONEXCLUSIVE | DISCL_BACKGROUND));
    if (!ok && exclusive) d.dev->SetCooperativeLevel(g.hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    d.exclusive = ok && exclusive;
    d.dev->Acquire();
    return ok;
}

// an effect on the steering's axis, made stopped; the type specific parameters are set as it runs
bool MakeEffect(IDirectInputDevice8W* dev, const GUID& guid, Effect& e, void* params, DWORD params_size, const char* name) {
    DWORD axes[1] = {kAxisOffsets[std::clamp(g.ffb_axis, 0, 7)]};
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
    if (FAILED(dev->CreateEffect(guid, &eff, &e.fx, nullptr)) || !e.fx) {
        e.fx = nullptr;
        return false;
    }
    e.fx->Start(1, 0);
    if (!g.effects.empty()) g.effects += ", ";
    g.effects += name;
    return true;
}

// the forces on the device and axis the steering is bound to, when it offers them
void PlaceFeedback(const Config& c) {
    std::vector<View> views;
    for (const Device& d : g.devs) views.push_back(d.view);
    int want = FindDevice(views, c.steer);
    if (want >= 0 && !views[size_t(want)].ffb) want = -1;
    int axis = std::clamp(c.steer.axis, 0, 7);
    if (want == g.ffb_dev && (want < 0 || axis == g.ffb_axis)) return;
    if (g.ffb_dev >= 0) {
        ReleaseEffects();
        SetMode(g.devs[size_t(g.ffb_dev)], false);
    }
    g.ffb_dev = want;
    g.ffb_axis = axis;
    if (want < 0) return;
    Device& d = g.devs[size_t(want)];
    if (!SetMode(d, true)) {
        LOGR("wheel: %s cannot be had exclusively (another program has it?): no force feedback", d.view.name.c_str());
        return;
    }
    // the device's own centring spring off: the forces below make one
    DIPROPDWORD z{};
    z.diph.dwSize = sizeof(z);
    z.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    z.diph.dwHow = DIPH_DEVICE;
    z.dwData = DIPROPAUTOCENTER_OFF;
    d.dev->SetProperty(DIPROP_AUTOCENTER, &z.diph);
    DICONDITION cond{};
    MakeEffect(d.dev, GUID_Spring, g.spring, &cond, sizeof(cond), "spring");
    MakeEffect(d.dev, GUID_Damper, g.damper, &cond, sizeof(cond), "damper");
    DICONSTANTFORCE constant{};
    MakeEffect(d.dev, GUID_ConstantForce, g.constant, &constant, sizeof(constant), "constant");
    DIPERIODIC periodic{};
    periodic.dwPeriod = 40000;  // microseconds
    MakeEffect(d.dev, GUID_Sine, g.sine, &periodic, sizeof(periodic), "sine");
    g.next_ffb = {};
    LOGR("wheel: force feedback on %s, axis %d: %s", d.view.name.c_str(), axis,
         g.effects.empty() ? "no effects made" : g.effects.c_str());
}

void Remove(size_t i) {
    Device& d = g.devs[i];
    LOGR("wheel: %s unplugged", d.view.name.c_str());
    if (int(i) == g.ffb_dev) ReleaseEffects(), g.ffb_dev = -1;
    else if (int(i) < g.ffb_dev) g.ffb_dev--;
    d.dev->Unacquire();
    d.dev->Release();
    g.devs.erase(g.devs.begin() + long(i));
}

// every two seconds: the game controllers plugged in that are not open yet
void Scan() {
    auto now = Clock::now();
    if (!g.di || now < g.next_scan) return;
    g.next_scan = now + std::chrono::seconds(2);
    std::vector<DIDEVICEINSTANCEW> found;
    if (FAILED(g.di->EnumDevices(DI8DEVCLASS_GAMECTRL, Collect, &found, DIEDFL_ATTACHEDONLY))) return;
    // an open device no longer attached is gone (the exclusive one only fails to acquire, never says unplugged)
    for (size_t i = 0; i < g.devs.size();) {
        bool attached = std::any_of(found.begin(), found.end(),
                                    [&](const DIDEVICEINSTANCEW& inst) { return GuidText(inst.guidInstance) == g.devs[i].view.guid; });
        if (attached) i++;
        else Remove(i);
    }
    for (const DIDEVICEINSTANCEW& inst : found) {
        std::string guid = GuidText(inst.guidInstance);
        if (g.skipped.count(guid)) continue;
        if (std::any_of(g.devs.begin(), g.devs.end(), [&](const Device& d) { return d.view.guid == guid; })) continue;
        IDirectInputDevice8W* dev = nullptr;
        if (FAILED(g.di->CreateDevice(inst.guidInstance, &dev, nullptr))) continue;
        std::string name = Narrow(inst.tszProductName);
        uint32_t vidpid = inst.guidProduct.Data1;
        if (IsXInput(dev)) {
            LOGR("wheel: %s (USB %04X:%04X) is an Xbox pad, left to the controller support", name.c_str(),
                 vidpid & 0xFFFF, vidpid >> 16);
            g.skipped.insert(guid);
            dev->Release();
            continue;
        }
        if (FAILED(dev->SetDataFormat(&c_dfDIJoystick2))) {
            dev->Release();
            continue;
        }
        Device d;
        d.dev = dev;
        d.view.name = name;
        d.view.guid = guid;
        d.view.type = inst.dwDevType;
        DIDEVCAPS caps{};
        caps.dwSize = sizeof(caps);
        dev->GetCapabilities(&caps);
        d.view.axes = int(caps.dwAxes);
        d.view.buttons = int(caps.dwButtons);
        d.view.povs = int(caps.dwPOVs);
        d.view.ffb = (caps.dwFlags & DIDC_FORCEFEEDBACK) != 0;
        AxisScan axes;
        dev->EnumObjects(CollectAxis, &axes, DIDFT_AXIS);
        d.view.axis_mask = axes.mask ? axes.mask : 0xFF;  // none listed: all eight looked at
        SetRange(dev);
        g.devs.push_back(d);
        SetMode(g.devs.back(), false);
        std::string axis_names;
        const char* kNames[8] = {"X", "Y", "Z", "Rx", "Ry", "Rz", "slider 1", "slider 2"};
        for (int a = 0; a < 8; a++)
            if (d.view.axis_mask >> a & 1) axis_names += std::string(axis_names.empty() ? "" : " ") + kNames[a];
        LOGR("wheel: %s (USB %04X:%04X, %s), %lu axes (%s), %lu buttons, %lu hats, force feedback %s", name.c_str(),
             vidpid & 0xFFFF, vidpid >> 16, TypeName(inst.dwDevType), caps.dwAxes, axis_names.c_str(), caps.dwButtons,
             caps.dwPOVs, d.view.ffb ? "offered" : "none");
    }
}

// each device's state, acquiring again after the window lost the focus; gone when unplugged or failing for long
void ReadAll() {
    for (size_t i = 0; i < g.devs.size();) {
        Device& d = g.devs[i];
        d.dev->Poll();
        HRESULT hr = d.dev->GetDeviceState(sizeof(d.view.state), &d.view.state);
        HRESULT acquired = S_OK;
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
            acquired = d.dev->Acquire();
            if (SUCCEEDED(acquired)) {
                d.dev->Poll();
                hr = d.dev->GetDeviceState(sizeof(d.view.state), &d.view.state);
                // effects stop when the device is lost: started again
                if (int(i) == g.ffb_dev)
                    for (Effect* e : {&g.spring, &g.damper, &g.constant, &g.sine})
                        if (e->fx) e->fx->Start(1, 0), e->last = -1;
            }
        }
        d.view.ok = SUCCEEDED(hr);
        // the exclusive one cannot be read while the window is behind another: that is not unplugged
        bool behind = d.exclusive && hr == DIERR_NOTACQUIRED && GetForegroundWindow() != g.hwnd;
        d.failures = d.view.ok || behind ? 0 : d.failures + 1;
        if (hr == DIERR_UNPLUGGED || acquired == DIERR_UNPLUGGED || d.failures > 500) {
            Remove(i);
            continue;
        }
        i++;
    }
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

// Forces, at most 60 times a second:
// - spring: centring, light in the menus, firmer with speed while racing (a car's self-aligning feel)
// - damper: a steady weight, so the wheel does not oscillate
// - sine: the game's rumble (kerbs, gravel, impacts), its low motor as a slow shake, its high one as a fine buzz
// - constant: the settings window's test pulse
void Feedback(const Shared& want, Clock::time_point now) {
    if (g.ffb_dev < 0 || now < g.next_ffb) return;
    g.next_ffb = now + std::chrono::milliseconds(16);
    const WheelFeel& feel = want.feel;
    float strength = std::clamp(float(want.config.ffb_strength) / 100.0f, 0.0f, 1.0f);
    bool paused = app::OverlayOpen() && now >= want.test_until;
    float speed = std::clamp(std::fabs(feel.speed) / 40.0f, 0.0f, 1.0f);  // full by 40 m/s, about 90 mph
    float spring = feel.racing ? 0.15f + 0.65f * speed : 0.25f;
    float damper = feel.racing ? 0.12f + 0.10f * speed : 0.10f;
    if (paused) spring = 0.25f, damper = 0.10f;
    SetCondition(g.spring, LONG(spring * strength * kForceMax));
    SetCondition(g.damper, LONG(damper * strength * kForceMax));
    float road = std::clamp(float(want.config.ffb_road) / 100.0f, 0.0f, 1.0f);
    float low = want.rumble_left / 65535.0f, high = want.rumble_right / 65535.0f;
    float shake = feel.racing && !paused ? std::max(low, high) * road * strength : 0;
    SetSine(LONG(shake * 0.6f * kForceMax), low >= high ? 60000 : 25000);
    float test = now < want.test_until ? want.test_level : 0;
    SetConstant(LONG(std::clamp(test, -1.0f, 1.0f) * strength * kForceMax));
}

// looks for devices every two seconds, reads them every 2 ms, sets the forces
void Run() {
    if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                  reinterpret_cast<void**>(&g.di), nullptr))) {
        LOGW("wheel: DirectInput unavailable, no steering wheels");
        g.di = nullptr;
        return;
    }
    Scan();
    if (g.devs.empty()) LOGR("wheel: no game controllers but Xbox pads (one plugged in later is picked up)");
    for (;;) {
        Scan();
        Config config;
        {
            std::lock_guard lock(g_lock);
            config = s.config;
        }
        PlaceFeedback(config);
        ReadAll();
        Shared want;
        {
            std::lock_guard lock(g_lock);
            s.devs.clear();
            for (const Device& d : g.devs) s.devs.push_back(d.view);
            s.ffb_dev = g.ffb_dev;
            s.effects = g.effects;
            if (g.ffb_dev < 0) s.feedback = false;
            want.config = s.config;
            want.feel = s.feel;
            want.rumble_left = s.rumble_left, want.rumble_right = s.rumble_right;
            want.feedback = s.feedback;
            want.test_until = s.test_until, want.test_level = s.test_level;
        }
        if (want.feedback) Feedback(want, Clock::now());
        Sleep(2);
    }
}

}  // namespace

void WheelSetup() {
    const app::Settings& st = app::Get();
    Config c;
    c.steer = st.wheel_steer;
    c.throttle = st.wheel_throttle;
    c.brake = st.wheel_brake;
    c.clutch = st.wheel_clutch;
    for (int i = 0; i < kControls && i < int(st.wheel_buttons.size()); i++) c.buttons[i] = st.wheel_buttons[i];
    for (int i = 0; i < app::kGears && i < int(st.wheel_gears.size()); i++) c.gears[i] = st.wheel_gears[i];
    c.range = st.wheel_range;
    c.lock = st.wheel_lock;
    c.deadzone = st.wheel_deadzone;
    c.ffb_strength = st.ffb_strength;
    c.ffb_road = st.ffb_road;
    std::lock_guard lock(g_lock);
    s.config = std::move(c);
}

void WheelInit(void* hwnd) {
    g.hwnd = static_cast<HWND>(hwnd);
    WheelSetup();
    std::thread(Run).detach();
}

bool WheelRead(float* v) {
    std::lock_guard lock(g_lock);
    const Config& c = s.config;
    bool active = false;
    if (const DIJOYSTATE2* st = StateOf(s.devs, c.steer)) {
        float steer = SteerValue(s.devs, c);
        v[kLSLeft] = std::max(-steer, 0.0f);
        v[kLSRight] = std::max(steer, 0.0f);
        active = true;
        // the steering device's hat is the d-pad, diagonals pressing both
        DWORD pov = st->rgdwPOV[0];
        if (LOWORD(pov) != 0xFFFF) {
            int deg = int(pov / 100) % 360;
            if (deg >= 315 || deg <= 45) v[kUp] = 1;
            if (deg >= 45 && deg <= 135) v[kRight] = 1;
            if (deg >= 135 && deg <= 225) v[kDown] = 1;
            if (deg >= 225 && deg <= 315) v[kLeft] = 1;
        }
    }
    if (StateOf(s.devs, c.throttle)) v[kRT] = PedalValue(s.devs, c.throttle), active = true;
    if (StateOf(s.devs, c.brake)) v[kLT] = PedalValue(s.devs, c.brake), active = true;
    for (int i = 0; i < kControls; i++) {
        if (!StateOf(s.devs, c.buttons[i])) continue;
        active = true;
        if (Pressed(s.devs, c.buttons[i])) v[i] = 1.0f;
    }
    // an H-pattern shifter: while its position differs from the car's gear, the game's own shift inputs are pressed,
    // one at a time, until they match (in neutral nothing is pressed)
    auto now = Clock::now();
    if (now < s.shift_until && s.shift_control >= 0) {
        v[s.shift_control] = 1.0f;
    } else {
        int want = SelectedGear(s.devs, c), have = s.feel.gear;
        if (want != kGearUnknown && have != kGearUnknown && want != have && now >= s.shift_next) {
            int control = want > have ? s.feel.shift_up : s.feel.shift_down;
            if (control >= 0 && control < kControls) {
                s.shift_control = control;
                s.shift_until = now + std::chrono::milliseconds(80);
                s.shift_next = now + std::chrono::milliseconds(250);
                v[control] = 1.0f;
            }
        }
    }
    return active;
}

void WheelFeedback(const WheelFeel& feel, uint16_t rumble_left, uint16_t rumble_right) {
    std::lock_guard lock(g_lock);
    s.feel = feel;
    s.rumble_left = rumble_left;
    s.rumble_right = rumble_right;
    s.feedback = true;
}

WheelInfo Wheel() {
    std::lock_guard lock(g_lock);
    WheelInfo w;
    for (size_t i = 0; i < s.devs.size(); i++) {
        const View& v = s.devs[i];
        WheelDevice d;
        d.name = v.name;
        d.guid = v.guid;
        d.ffb = v.ffb;
        d.feedback = int(i) == s.ffb_dev;
        d.axes_count = v.axes;
        d.axis_mask = v.axis_mask;
        d.buttons_count = v.buttons;
        d.povs_count = v.povs;
        if (v.ok) {
            for (int a = 0; a < 8; a++) d.axes[a] = Axis(v.state, a);
            for (int b = 0; b < 128; b++)
                if (v.state.rgbButtons[b] & 0x80) d.pressed.push_back(b);
            if (LOWORD(v.state.rgdwPOV[0]) != 0xFFFF) d.pov = int(v.state.rgdwPOV[0]);
        }
        w.devices.push_back(std::move(d));
    }
    const Config& c = s.config;
    w.active = StateOf(s.devs, c.steer) || StateOf(s.devs, c.throttle) || StateOf(s.devs, c.brake);
    w.effects = s.effects;
    w.steer = SteerValue(s.devs, c);
    w.throttle = PedalValue(s.devs, c.throttle);
    w.brake = PedalValue(s.devs, c.brake);
    w.clutch = PedalValue(s.devs, c.clutch);
    w.gear_selected = SelectedGear(s.devs, c);
    w.gear_game = s.feel.gear;
    return w;
}

void WheelTestForce(float level) {
    std::lock_guard lock(g_lock);
    s.test_level = level;
    s.test_until = Clock::now() + std::chrono::milliseconds(500);
    s.feedback = true;
}

}  // namespace input
