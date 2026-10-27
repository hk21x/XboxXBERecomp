// the host: window, message loop, crash dumps; the game is described by app::kGame
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <thread>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "core/diag.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace app {

namespace {

HWND g_hwnd = nullptr;
constexpr UINT kApplyDisplay = WM_APP + 1, kOpenWindow = WM_APP + 2;
bool g_mode_changed = false;
bool g_apply_posted = false;  // a display apply is waiting in the queue or running
std::atomic<bool> g_active{true};  // the game window is in front (WM_ACTIVATE)

// crash dump in CrashDumps beside the exe
LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS* e) {
    // the dump holds no guest memory: the guest registers and return addresses go to the log
    if (kernel::Thread* t = kernel::Current(); e && t && t->ctx) {
        auto* r = e->ExceptionRecord;
        LOGE("exception %08lX at %p on guest thread %X", r->ExceptionCode, r->ExceptionAddress, t->id);
        if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
            uint64_t va = r->ExceptionInformation[1], g = uint64_t(reinterpret_cast<uintptr_t>(kernel::g_base));
            LOGE("  %s %llX, guest %08llX", r->ExceptionInformation[0] ? "write" : "read", (unsigned long long)va,
                 (unsigned long long)(va - g));
        }
        kernel::LogGuestStack(*t->ctx, 256);
    }
    auto dir = ExeDir() / "CrashDumps";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf(name, 64, L"%ls.%04u.%02u.%02u_%02u.%02u.%02u.dmp", kGame.name, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    HANDLE f = CreateFileW((dir / name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), e, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                      MINIDUMP_TYPE(MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                                    MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory),
                      e ? &info : nullptr, nullptr, nullptr);
    CloseHandle(f);
    logx::Flush();
    return EXCEPTION_CONTINUE_SEARCH;
}

// F3 with XBE_CAPTURE_DIR: the guest's committed memory too, for a stalled title's state to be read elsewhere
// (sdk/scripts/guestmem.py): "XBEMEM1" and a zero byte, then runs of committed pages, each its address, its length
// and its bytes
void DumpGuestMemory() {
    if (diag::CaptureDir().empty()) return;
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::weakly_canonical(ExeDir() / diag::CaptureDir(), ec);
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path file = dir / ("guest_mem_" + std::to_string(GetTickCount64()) + ".bin");
    FILE* f = _wfopen(file.wstring().c_str(), L"wb");
    if (!f) return;
    std::fwrite("XBEMEM1", 1, 8, f);
    uint64_t total = 0;
    for (uint64_t va = 0x1000; va < 0x100000000ull;) {
        if (!mem::IsCommitted(uint32_t(va))) {
            va += 0x1000;
            continue;
        }
        uint64_t end = va;
        while (end < 0x100000000ull && end - va < 0x1000000 && mem::IsCommitted(uint32_t(end))) end += 0x1000;
        uint32_t head[2] = {uint32_t(va), uint32_t(end - va)};
        std::fwrite(head, 4, 2, f);
        std::fwrite(kernel::HostPtr(uint32_t(va)), 1, size_t(end - va), f);
        total += end - va;
        va = end;
    }
    std::fclose(f);
    LOGR("F3: guest memory, %llu MB, written to %s", (unsigned long long)(total >> 20), file.string().c_str());
}

void DumpThreads() {
    LOGR("F3: thread stacks%s", diag::CaptureDir().empty() ? " (no capture: XBE_CAPTURE_DIR is not set)" : ", the next frame captured");
    gpu::g_capture = true;
    for (auto& t : kernel::Threads())
        if (t->ctx) {
            LOGR("F3: thread %X", t->id);
            kernel::LogGuestStack(*t->ctx, 32);
        }
    DumpGuestMemory();
}

// window thread only
void ApplyNow() {
    const Settings& s = Get();
    HMONITOR mon = static_cast<HMONITOR>(Monitor());
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    if (g_mode_changed) {
        ChangeDisplaySettingsExW(mi.szDevice, nullptr, nullptr, 0, nullptr);
        g_mode_changed = false;
    }
    Mode out = Output();
    LONG_PTR visible = IsWindowVisible(g_hwnd) ? WS_VISIBLE : 0;  // a window held back for a splash or handover stays so
    DEVMODEW desktop{};
    desktop.dmSize = sizeof(desktop);
    EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &desktop);
    // the desktop's own size is no mode change: switching to it anyway blanks the screen for nothing
    if (s.display_mode == DisplayMode::Fullscreen && s.width && s.height &&
        (s.width != desktop.dmPelsWidth || s.height != desktop.dmPelsHeight)) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        dm.dmPelsWidth = s.width;
        dm.dmPelsHeight = s.height;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        g_mode_changed = ChangeDisplaySettingsExW(mi.szDevice, &dm, nullptr, CDS_FULLSCREEN, nullptr) == DISP_CHANGE_SUCCESSFUL;
        GetMonitorInfoW(mon, &mi);
    }
    if (s.display_mode == DisplayMode::Windowed) {
        RECT r{0, 0, LONG(out.width), LONG(out.height)};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        int w = r.right - r.left, h = r.bottom - r.top;
        const RECT& a = mi.rcWork;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | visible);
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, a.left + (a.right - a.left - w) / 2, a.top + (a.bottom - a.top - h) / 2, w, h,
                     SWP_FRAMECHANGED);
    } else {
        const RECT& m = mi.rcMonitor;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | visible);
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED);
    }
}

// mouse look (Settings::mouse_look): the cursor hidden and held in the window while the mouse plays the right stick
void HoldCursor(HWND hwnd) {
    static bool held = false;
    bool want = input::MouseLooking();
    if (want) {
        RECT r;
        GetClientRect(hwnd, &r);
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
        ClipCursor(&r);
    } else if (held) {
        ClipCursor(nullptr);
    }
    held = want;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // mouse look: raw movement, the cursor kept hidden in the window
    if (msg == WM_INPUT && kGame.mouse_look) {
        RAWINPUT ri;
        UINT size = sizeof(ri);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
            ri.header.dwType == RIM_TYPEMOUSE && input::MouseLooking()) {
            if (!(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) input::MouseMoved(ri.data.mouse.lLastX, ri.data.mouse.lLastY);
            if (ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) input::MouseWheel(short(ri.data.mouse.usButtonData));
        }
        HoldCursor(hwnd);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (msg == WM_SETCURSOR && kGame.mouse_look && LOWORD(lp) == HTCLIENT) {
        HoldCursor(hwnd);
        if (input::MouseLooking()) {
            SetCursor(nullptr);
            return TRUE;
        }
    }
    // a key binding the settings window waits for takes the next mouse button too
    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN) {
        uint16_t vk = msg == WM_LBUTTONDOWN   ? VK_LBUTTON
                      : msg == WM_RBUTTONDOWN ? VK_RBUTTON
                      : msg == WM_MBUTTONDOWN ? VK_MBUTTON
                      : GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? VK_XBUTTON1
                                                           : VK_XBUTTON2;
        std::lock_guard lock(UiLock());
        if (OverlayKey(vk)) return 0;
    }
    // the settings window: F1 opens and closes it, and a key binding it waits for takes the next key
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        std::lock_guard lock(UiLock());
        if (wp == VK_F1 && !(lp & (1 << 30))) {
            ToggleOverlay();
            return 0;
        }
        if (OverlayKey(uint16_t(wp))) return 0;
    }
    {
        std::lock_guard lock(UiLock());
        if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    }
    if (intptr_t result = 0; kGame.message && kGame.message(hwnd, msg, wp, lp, &result)) return result;
    switch (msg) {
        case WM_KEYDOWN:
            if (wp == VK_F3) DumpThreads();
            if (wp == VK_F4) {
                LOGR("F4: frame log requested");
                diag::g_framelog_request = true;
            }
            return 0;
        case WM_ACTIVATE:
            g_active = LOWORD(wp) != WA_INACTIVE;
            if (LOWORD(wp) == WA_INACTIVE) {
                // a changed display mode steps aside for the desktop, minimized, and comes back on return; not while
                // handing over to the next instance, which runs in the same mode
                if (g_mode_changed && !kernel::g_relaunching) {
                    MONITORINFOEXW mi{};
                    mi.cbSize = sizeof(mi);
                    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
                    ChangeDisplaySettingsExW(mi.szDevice, nullptr, nullptr, 0, nullptr);
                    g_mode_changed = false;
                    ShowWindow(hwnd, SW_MINIMIZE);
                }
            } else if (Get().display_mode == DisplayMode::Fullscreen && Get().width && Get().height && !g_mode_changed &&
                       IsWindowVisible(hwnd) && !g_apply_posted) {
                // once: restoring the window activates it again from inside the apply, and a second apply switched
                // the mode back and forth, after which presents were not taken for a while (the game stood still
                // until the next alt-tab)
                g_apply_posted = true;
                PostMessageW(hwnd, kApplyDisplay, 0, 0);
            }
            break;
        case kApplyDisplay:
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            ApplyNow();
            g_apply_posted = false;
            return 0;
        case kOpenWindow:
            CloseSplash();
            if (wp) ApplyNow();
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

std::filesystem::path ExeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return std::filesystem::path(p).parent_path();
}

int Instance() {
    static const int n = [] {
        const wchar_t* p = std::wcsstr(GetCommandLineW(), L"--instance");
        return p ? std::max(0, std::min(8, _wtoi(p + 10))) : 0;
    }();
    return n;
}

void* Window() { return g_hwnd; }

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void ApplyDisplay() { PostMessageW(g_hwnd, kApplyDisplay, 0, 0); }
void Quit() {
    LOGR("quit asked for");
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}
void OpenGameWindow(bool apply) { SendMessageW(g_hwnd, kOpenWindow, apply, 0); }

bool Paused() { return Get().pause_unfocused && !g_active && g_hwnd && IsWindowVisible(g_hwnd); }

void Pace(uint32_t vblanks) {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    // pause_unfocused: the title waits here, the sound output stops pulling the mixer (so the title's sample clock
    // stands still too)
    if (Paused()) {
        LOGR("paused: the window is not in front");
        while (Paused()) Sleep(10);
        LOGR("resumed");
        next = clock::now();
    }
    uint32_t cap = PaceRate(Get().fps_cap, vblanks);
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    // stutter: frames that took more than one and a half periods, a line every 10 seconds that had any (loading
    // screens count too)
    static clock::time_point last = now, report = now + std::chrono::seconds(10);
    static uint32_t frames = 0, slow = 0;
    static double longest = 0;
    double took = std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    frames++;
    if (took > 1.5 * std::chrono::duration<double, std::milli>(period).count()) slow++, longest = std::max(longest, took);
    if (now >= report) {
        // shaders compiled the first time a draw needed them: the usual cause of a stall past loading
        uint32_t compiled = gpu::g_shaders_compiled.exchange(0);
        if (slow) LOGR("frames: %u of %u in the last 10 s over %.1f ms (%u a second paced), the longest %.1f ms; "
                       "%u shaders compiled", slow, frames, 1.5 * std::chrono::duration<double, std::milli>(period).count(),
                       cap, longest, compiled);
        report = now + std::chrono::seconds(10);
        frames = slow = 0;
        longest = 0;
    }
    static int64_t slot = INT64_MIN / 2;
    if (60 % cap == 0) {
        next = PaceOnBlanks(slot, cap, now);
    } else {
        next += period;
        if (next < now - period) next = now;
    }
    while (clock::now() + std::chrono::milliseconds(2) < next) Sleep(1);
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace app

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetUnhandledExceptionFilter(app::WriteCrashDump);
    std::signal(SIGABRT, [](int) { app::WriteCrashDump(nullptr); });
    timeBeginPeriod(1);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // one log a session: a relaunch into another image carries on its predecessor's
    bool relaunched = std::wcsstr(GetCommandLineW(), L"--relaunched") != nullptr;
    std::string log = app::kGame.log;
    if (app::Instance() > 1) log += "-" + std::to_string(app::Instance());
    logx::Init((app::ExeDir() / "logs").string(), log, relaunched);
    app::Load();
    if (app::kGame.init) app::kGame.init();
    if (relaunched && !logx::kVerbose) LOGR("Relaunched: %s", IMAGE_XBE);
    else app::LogSystem();

    app::GameFiles game;
    if (!app::FindGame(game)) return 1;

    bool splash = app::OpenSplash(), handover = app::StartHandover();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = app::WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = app::kGame.name;
    RegisterClassExW(&wc);
    app::g_hwnd = CreateWindowExW(0, wc.lpszClassName, app::kGame.title, WS_POPUP, 0, 0, 1280, 720,
                                  nullptr, nullptr, inst, nullptr);
    // mouse look: the mouse's raw movement (WM_INPUT), not the cursor's, which stops at the screen's edge
    if (app::kGame.mouse_look) {
        RAWINPUTDEVICE rid{0x01, 0x02, 0, app::g_hwnd};  // generic desktop, mouse
        if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) LOGW("input: raw mouse input not registered (%lu)", GetLastError());
    }
    // behind a splash the window stays hidden until the game's first frame; handed over, it is set up hidden
    if (handover) {
        app::ApplyNow();
    } else if (!splash) {
        app::ApplyNow();
        ShowWindow(app::g_hwnd, SW_SHOW);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplWin32_Init(app::g_hwnd);

    input::Init();
    if (app::kGame.window) app::kGame.window(app::g_hwnd);
    std::wstring hdd = app::Instance() > 1 ? L"hdd" + std::to_wstring(app::Instance()) : L"hdd";
    if (app::Instance())
        SetWindowTextW(app::g_hwnd, (std::wstring(app::kGame.title) + L" [" + std::to_wstring(app::Instance()) + L"]").c_str());
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / hdd).string())) {
        MessageBoxW(app::g_hwnd, L"The game failed to start, see the log in the logs folder.", app::kGame.short_title,
                    MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (app::g_mode_changed) ChangeDisplaySettingsW(nullptr, 0);
    LOGR("the window closed");
    logx::Flush();
    kernel::Shutdown(0);
}
