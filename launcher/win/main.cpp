// The launcher's window: choose the game (a disc image or a folder), build it on this PC with portable tools, play.
// Dear ImGui on Direct3D 11; the work runs on a thread of its own (core/builder.h), the window shows its status.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "screen.h"
#include "core/builder.h"
#include "core/update.h"
#include "core/util.h"
#include "embedded.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace fs = std::filesystem;
using namespace launcher;

namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resize_w = 0, g_resize_h = 0;

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

void MakeTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

bool MakeDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                               &sd, &g_swap, &g_device, &level, &g_context);
    if (FAILED(hr))  // no GPU driver: the software rasteriser draws this window well enough
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                           &g_swap, &g_device, &level, &g_context);
    if (FAILED(hr)) return false;
    MakeTarget();
    return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) g_resize_w = LOWORD(lp), g_resize_h = HIWORD(lp);
            return 0;
        case WM_GETMINMAXINFO: {
            UINT dpi = GetDpiForWindow(hwnd);
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize = {LONG(640 * dpi / 96), LONG(480 * dpi / 96)};
            return 0;
        }
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

namespace launcher {

// the shell's own picker: a disc image (or the xbe), or a folder
fs::path Pick(void* owner_window, bool folder, const wchar_t* title) {
    HWND owner = static_cast<HWND>(owner_window);
    fs::path out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
    if (folder) {
        dlg->SetTitle(title ? title : L"Choose the folder with the game's files");
    } else {
        COMDLG_FILTERSPEC types[] = {{L"Xbox disc image or default.xbe", L"*.iso;default.xbe"}, {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, types);
        dlg->SetTitle(L"Choose the game's disc image");
    }
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            wchar_t* path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                out = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

void OpenUrl(const std::string& url) { ShellExecuteW(nullptr, L"open", Wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
void OpenFolder(const fs::path& dir) { ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

uint64_t FreeBytes(const fs::path& dir) {
    ULARGE_INTEGER avail{};
    fs::path d = dir;
    std::error_code ec;
    while (!d.empty() && !fs::exists(d, ec)) d = d.parent_path();
    GetDiskFreeSpaceExW(d.c_str(), &avail, nullptr, nullptr);
    return avail.QuadPart;
}

}  // namespace launcher

namespace {

// this exe's path, and the names an update uses beside it
fs::path SelfPath() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    return self;
}
fs::path Beside(const fs::path& exe, const wchar_t* ext) {
    fs::path p = exe;
    p += ext;
    return p;
}

// The release's launcher downloaded beside this one (checked by its SHA-256 where GitHub lists it, else its size),
// then swapped in: Windows lets a running exe be renamed, not overwritten, so this one becomes <exe>.old, which the
// next start removes. The new one starts when this window closes (App::restart).
}  // namespace

bool launcher::UpdateLauncher(App& app, const Release& r, std::string& error) {
    fs::path exe = SelfPath(), fresh = Beside(exe, L".new"), old = Beside(exe, L".old");
    app.builder->cancel = false;
    Progress progress = [&app](uint64_t done, uint64_t total) {
        std::lock_guard l(app.lock);
        app.status = {"Downloading the new launcher", Megabytes(done) + " of " + Megabytes(total),
                      total ? double(done) / double(total) : -1};
    };
    if (!Download(*app.http, r.launcher_url, fresh, r.launcher_sha256, r.launcher_size, progress, &app.builder->cancel,
                  error))
        return false;
    std::error_code ec;
    fs::remove(old, ec);
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = "The launcher could not be updated (it could not rename itself). The new one is " + U8(fresh) + ".";
        return false;
    }
    if (!MoveFileExW(fresh.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        error = "The launcher could not be updated (the new one could not take its place). It is " + U8(fresh) + ".";
        return false;
    }
    app.restart = true;
    return true;
}


int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    App app;
    std::string e;
    if (!ParseGameConfig(kGameJson, app.game, e)) {
        MessageBoxW(nullptr, Wide(e).c_str(), L"Launcher", MB_ICONERROR);
        return 1;
    }
    app.game.version = kLauncherVersion;
    // what an update left: the launcher it replaced (still running then, so removed now)
    {
        std::error_code rm;
        fs::remove(Beside(SelfPath(), L".old"), rm);
    }
    // the work folder beside the launcher, named after the game
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    app.root = fs::path(self).parent_path() / app.game.short_name;
    app.builder = std::make_unique<Builder>(app.game, app.root, *app.http, *app.platform);
    app.builder->fallback_toolchain = kToolchainJson;
    app.builder->on_status = [&app](const Status& s) {
        std::lock_guard l(app.lock);
        app.status = s;
    };
    app.builder->on_log = [&app](const std::string& line) {
        std::lock_guard l(app.lock);
        app.log.push_back(line);
        if (app.log.size() > 2000) app.log.pop_front();
    };
    std::error_code ec;
    app.agreed = fs::exists(app.root / "agreed.txt", ec);
    fs::path saved = app.builder->SavedGame();
    if (!saved.empty()) app.copy = CheckGame(app.game, saved);
    app.music = app.builder->SavedMusic();
    app.build_bytes = app.builder->BuildFilesBytes();
    app.old_bytes = app.builder->OldBuildBytes();
    // a new version: a newer release's launcher looked for once at start (a release launcher only), and the game
    // when it was built before
    app.Start([&app](std::string& err) {
        if (IsReleaseVersion(app.game.version)) {
            Release r;
            std::string e;
            // what the check found, in launcher.log: a launcher that offers nothing says why
            if (!LatestRelease(*app.http, app.game, r, e)) {
                app.builder->Log("update check: " + (e.empty() ? std::string("no answer") : e));
            } else if (CompareVersions(r.tag, app.game.version) > 0) {
                app.builder->Log("update check: " + r.tag + " is out, this launcher is " + app.game.version + ": offered");
                std::lock_guard l(app.release_lock);
                app.release = r;
                app.launcher_update = true;
            } else {
                app.builder->Log("update check: the latest release is " + r.tag + ", this launcher is " + app.game.version);
            }
        }
        bool available = false;
        if (app.builder->Built() && app.builder->UpdateAvailable(available, err)) app.update = available;
        err.clear();  // offline: no news is fine
        return true;
    }, "");

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"XbeRecompLauncher";
    RegisterClassExW(&wc);
    std::wstring title = Wide(app.game.title) + L" - Launcher";
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                100, 100, nullptr, nullptr, inst, nullptr);
    float scale = float(GetDpiForWindow(hwnd)) / 96.0f;
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = int(900 * scale), h = int(700 * scale);
    SetWindowPos(hwnd, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2,
                 w, h, SWP_NOZORDER);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    if (!MakeDevice(hwnd)) {
        MessageBoxW(hwnd, L"Direct3D 11 could not start.", title.c_str(), MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ScreenStyle(scale);
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    ImFont* big = ScreenFonts(fs::path(windir) / "Fonts", scale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    for (bool run = true; run;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) run = false;
        }
        if (!run) break;
        if (g_resize_w) {
            if (g_rtv) g_rtv->Release(), g_rtv = nullptr;
            g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            MakeTarget();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawScreen(app, hwnd, big);
        ImGui::Render();
        const float clear[4] = {62 / 255.0f, 70 / 255.0f, 55 / 255.0f, 1};  // classic::kWindow
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
        // the launcher updated itself: the new one starts, this one closes
        if (app.restart && !app.busy) {
            fs::path exe = SelfPath();
            std::string err;
            if (app.platform->Start(exe, exe.parent_path(), err)) break;
            app.restart = false;
            std::lock_guard l(app.lock);
            app.error = "The new launcher is in place but did not start: " + err + " Start it again.";
        }
        if (!app.busy && !IsWindowVisible(hwnd)) Sleep(50);
    }
    // closing during a build stops it
    app.builder->cancel = true;
    if (app.worker.joinable()) app.worker.join();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release();
    if (g_swap) g_swap->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    CoUninitialize();
    return 0;
}
