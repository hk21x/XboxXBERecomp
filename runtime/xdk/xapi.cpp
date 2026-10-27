// XAPI functions that talk to hardware: controller ports and the hard disk cache partition
#include <chrono>

#include "core/input.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;

// configs written before the SDK knew the name
#ifndef SYM_XapiVerifyMediaInDrive
#define SYM_XapiVerifyMediaInDrive FFFFFC00
#define HAS_XapiVerifyMediaInDrive 0
#endif
#ifndef SYM_XInputGetCapabilities
#define SYM_XInputGetCapabilities FFFFFC04
#define HAS_XInputGetCapabilities 0
#endif

namespace {

// XPP device type records: CurrentConnected, ChangeConnected, PreviousConnected
constexpr uint32_t kGamepadType = SYM(XPP_GamepadType);
constexpr uint32_t kMemoryUnitType = SYM(XPP_MemoryUnitType);

std::mutex g_ports_lock;
bool g_poller = false;
uint32_t g_wheels = 0;  // ports a steering wheel steers, as XInputGetCapabilities last told

void UpdatePorts() {
    std::lock_guard<std::mutex> lock(g_ports_lock);
    uint32_t cur = 0, wheels = 0;
    for (int p = 0; p < 4; p++) {
        input::Pad pad = input::Poll(p);
        if (pad.connected) cur |= 1u << p;
        if (pad.connected && pad.wheel) wheels |= 1u << p;
    }
    uint32_t old = R32(kGamepadType + XDK_XPP_Current);
    // a port that became or stopped being a wheel is unplugged and plugged again (XGetDeviceChanges reports both),
    // so the title opens it again and reads its capabilities
    uint32_t replug = (wheels ^ g_wheels) & cur & old;
    if (cur != old || replug) {
        W32(kGamepadType + XDK_XPP_Changed, R32(kGamepadType + XDK_XPP_Changed) | (cur ^ old) | replug);
        W32(kGamepadType + XDK_XPP_Current, cur);
        LOGI("xapi: controller ports now %X, wheels %X", cur, wheels);
    }
    g_wheels = wheels;
}

struct Handle {
    uint32_t port;
    uint32_t packet;
    input::Pad last;
};
std::unordered_map<uint32_t, Handle> g_handles;

}  // namespace

HLE_STDCALL(XMountUtilityDrive, 1) {
    CreateSymbolicLink("\\??\\Z:", "\\Device\\Harddisk0\\Partition5");
    return 1;
}

HLE_STDCALL(XInitDevices, 2) {
    XDK_REQUIRE("xapi", XDK_KEY(XPP_Current), XDK_KEY(XPP_Changed), XDK_KEY(XPP_Previous), XDK_DATA(XPP_GamepadType));
    W32(kGamepadType + XDK_XPP_Current, 0);
    W32(kGamepadType + XDK_XPP_Changed, 0);
    W32(kGamepadType + XDK_XPP_Previous, 0);
    if constexpr (HAS_XPP_MemoryUnitType != 0) {  // titles without memory unit support have no record
        W32(kMemoryUnitType + XDK_XPP_Current, 0);
        W32(kMemoryUnitType + XDK_XPP_Changed, 0);
        W32(kMemoryUnitType + XDK_XPP_Previous, 0);
    }
    UpdatePorts();
    if (!g_poller) {
        g_poller = true;
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                UpdatePorts();
            }
        }).detach();
    }
    return 0;
}

HLE_STDCALL(XInputOpen, 4) {
    uint32_t type = a[0], port = a[1];
    if (type != kGamepadType || port > 3) return 0;
    uint32_t h = mem::AllocSystem(16);
    std::lock_guard<std::mutex> lock(g_ports_lock);
    g_handles[h] = {port, 0, {}};
    return h;
}

HLE_STDCALL(XInputClose, 1) {
    std::lock_guard<std::mutex> lock(g_ports_lock);
    g_handles.erase(a[0]);
    mem::FreeSystem(a[0]);
    return 0;
}

HLE_STDCALL(XInputGetState, 2) {
    uint32_t port;
    {
        std::lock_guard<std::mutex> lock(g_ports_lock);
        auto it = g_handles.find(a[0]);
        if (it == g_handles.end()) return 6;  // ERROR_INVALID_HANDLE
        port = it->second.port;
    }
    input::Pad p = input::Poll(int(port));
    if (!p.connected) return 1167;  // ERROR_DEVICE_NOT_CONNECTED
    std::lock_guard<std::mutex> lock(g_ports_lock);
    Handle& h = g_handles[a[0]];
    if (std::memcmp(&p, &h.last, sizeof(p))) h.packet++;
    h.last = p;
    uint32_t s = a[1];
    W32(s, h.packet);  // XINPUT_STATE: packet, buttons, 8 analog buttons, thumbs
    W16(s + 4, p.buttons);
    std::memcpy(HostPtr(s + 6), p.analog, 8);
    W16(s + 14, uint16_t(p.lx));
    W16(s + 16, uint16_t(p.ly));
    W16(s + 18, uint16_t(p.rx));
    W16(s + 20, uint16_t(p.ry));
    return 0;
}

// XINPUT_CAPABILITIES: SubType, Reserved, In (an XINPUT_GAMEPAD of what it has), Out (XINPUT_RUMBLE). A title may
// treat a wheel (XINPUT_DEVSUBTYPE_GC_WHEEL) differently, PGR2 without its gamepad dead zone and stick shaping
HLE_STDCALL(XInputGetCapabilities, 2) {
    uint32_t port;
    {
        std::lock_guard<std::mutex> lock(g_ports_lock);
        auto it = g_handles.find(a[0]);
        if (it == g_handles.end()) return 6;
        port = it->second.port;
    }
    input::Pad p = input::Poll(int(port));
    if (!p.connected) return 1167;
    uint32_t c = a[1];
    std::memset(HostPtr(c), 0, 24);
    G<uint8_t>(c) = p.wheel ? 0x10 : 0x01;  // XINPUT_DEVSUBTYPE_GC_WHEEL, XINPUT_DEVSUBTYPE_GC_GAMEPAD
    W16(c + 2, 0xFF);
    std::memset(HostPtr(c + 4), 0xFF, 8);
    for (int i = 0; i < 4; i++) W16(c + 12 + i * 2, 0xFFFF);
    W16(c + 20, 0xFFFF);
    W16(c + 22, 0xFFFF);
    {
        std::lock_guard<std::mutex> lock(g_ports_lock);
        if (p.wheel) g_wheels |= 1u << port;
        else g_wheels &= ~(1u << port);
    }
    LOGI("xapi: controller %u is a %s", port + 1, p.wheel ? "steering wheel" : "gamepad");
    return 0;
}

HLE_STDCALL(XInputSetState, 2) {
    uint32_t port;
    {
        std::lock_guard<std::mutex> lock(g_ports_lock);
        auto it = g_handles.find(a[0]);
        if (it == g_handles.end()) return 6;
        port = it->second.port;
    }
    uint32_t fb = a[1];
    // XINPUT_FEEDBACK_HEADER is dwStatus, hEvent and 58 reserved bytes, the rumble follows
    input::Rumble(int(port), R16(fb + 66), R16(fb + 68));
    W32(fb, 0);
    if (uint32_t ev = R32(fb + 4))
        if (ObjectRef o = FromHandle(ev)) SetEvent(*o);
    return 997;  // ERROR_IO_PENDING, already complete
}

// A recompiled image launches through the XAPI, then a process restart into its executable; any other
// (the dashboard, updates) is refused and the title stays in its menu.
X86_EXTERN_FUNC(GUEST_ORIG(XLaunchNewImageA));
X86_FUNC(GUEST(XLaunchNewImageA)) {
    std::string path = ReadCString(R32(ctx.esp + 4));
    if (ImageExe(path)) {
        GUEST_ORIG(XLaunchNewImageA)(ctx, base);
        return;
    }
    LOGW("xapi: launch of %s refused, it is not recompiled", path.c_str());
    Return(ctx, 2, 2);  // ERROR_FILE_NOT_FOUND
}

// XAPI's startup check of the disc in the drive (the name is ours, the XDK's is not known): an authentication mode
// page, then for titles whose certificate asks for it the signed security sector. The game runs from the player's
// own files, so there is no drive or disc to authenticate; the check passes as it does with the disc inserted.
HLE_STDCALL(XapiVerifyMediaInDrive, 0) { return 0; }
