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

constexpr const char* kLicenseUrl = "https://go.microsoft.com/fwlink/?LinkId=2179911";  // Visual Studio Build Tools

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

// the shell's own picker: a disc image (or the xbe), or a folder
fs::path Pick(HWND owner, bool folder, const wchar_t* title = nullptr) {
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

// The window's state; the worker thread writes status and log under the lock
struct App {
    GameConfig game;
    std::unique_ptr<Http> http = MakeHttp();
    std::unique_ptr<Platform> platform = MakePlatform();
    std::unique_ptr<Builder> builder;
    fs::path root;

    GameCheck copy;
    bool agreed = false;
    std::atomic<bool> busy{false}, update{false}, checked_update{false};
    // a newer release's launcher (core/update.h): offered at the top until updated or put off
    std::mutex release_lock;
    Release release;
    std::atomic<bool> launcher_update{false}, later{false}, restart{false};
    std::thread worker;
    std::mutex lock;
    Status status;
    std::string error, done_message;
    std::deque<std::string> log;
    uint64_t build_bytes = 0;
    fs::path music;            // the music folder imported last
    bool music_job = false;    // the job running or last run imports the music: its result shows under Your music
    std::string music_summary;

    void Start(std::function<bool(std::string&)> job, std::string done) {
        if (busy) return;
        if (worker.joinable()) worker.join();
        busy = true;
        {
            std::lock_guard l(lock);
            error.clear();
            done_message.clear();
            status = {};
        }
        worker = std::thread([this, job, done] {
            std::string e;
            bool ok = job(e);
            std::lock_guard l(lock);
            if (!ok) error = e == "cancelled" ? "Stopped. Press the button again to carry on where it left off." : e;
            else done_message = done;
            status = {};
            build_bytes = builder->BuildFilesBytes();
            busy = false;
        });
    }
};

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
bool UpdateLauncher(App& app, const Release& r, std::string& error) {
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

void Section(const char* title) {
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.2f, 1), "%s", title);
    ImGui::Separator();
    ImGui::Spacing();
}

void Draw(App& app, HWND hwnd, ImFont* big) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    float em = ImGui::GetFontSize();
    ImGui::PushFont(big);
    ImGui::TextUnformatted(app.game.title.c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("PC port launcher. Everything goes in %s", U8(app.root).c_str());

    bool busy = app.busy;
    bool built = app.builder->Built();

    if (app.launcher_update && !app.later) {
        Release r;
        {
            std::lock_guard l(app.release_lock);
            r = app.release;
        }
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.75f, 1, 1), "Version %s is out (this launcher is %s).", r.tag.c_str(),
                           app.game.version.c_str());
        ImGui::TextWrapped("Update the launcher, then build the game again with the new version. Your saves and "
                           "settings are kept.");
        ImGui::BeginDisabled(busy || r.launcher_url.empty());
        if (ImGui::Button("Update the launcher")) {
            app.music_job = false;
            app.Start([&app, r](std::string& e) { return UpdateLauncher(app, r, e); }, "");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("What's new") && !r.page.empty()) OpenUrl(r.page);
        ImGui::SameLine();
        if (ImGui::Button("Later")) app.later = true;
    }

    Section("1  Your game");
    if (app.copy.ok) {
        ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1), "%s", app.copy.release.c_str());
        ImGui::SameLine();
        ImGui::TextWrapped("%s", U8(app.copy.path).c_str());
    } else {
        ImGui::TextWrapped("Choose your copy of the game: a disc image (.iso) of your own disc, or a folder with the "
                           "disc's files. Nothing is copied off the internet: the game comes from your disc.");
        if (!app.copy.error.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", app.copy.error.c_str());
    }
    ImGui::BeginDisabled(busy);
    for (int folder = 0; folder < 2; folder++) {
        if (folder) ImGui::SameLine();
        if (ImGui::Button(folder ? "Choose a folder..." : "Choose a disc image...")) {
            fs::path p = Pick(hwnd, folder != 0);
            if (!p.empty()) {
                app.copy = CheckGame(app.game, p);
                std::string e;
                if (app.copy.ok && !app.builder->SetGame(app.copy, e)) app.copy.ok = false, app.copy.error = e;
            }
        }
    }
    ImGui::EndDisabled();

    Section("2  Build");
    std::string error, done;
    Status st;
    {
        std::lock_guard l(app.lock);
        error = app.error;
        done = app.done_message;
        st = app.status;
    }
    if (busy) {
        ImGui::TextUnformatted(st.step.empty() ? "Starting..." : st.step.c_str());
        if (st.fraction >= 0) ImGui::ProgressBar(float(st.fraction), ImVec2(-1, 0), st.detail.empty() ? nullptr : st.detail.c_str());
        else {
            ImGui::ProgressBar(-float(ImGui::GetTime()), ImVec2(-1, 0), "");
            if (!st.detail.empty()) ImGui::TextDisabled("%s", st.detail.c_str());
        }
        if (ImGui::Button("Stop")) app.builder->cancel = true;
        ImGui::SameLine();
        ImGui::TextDisabled("It carries on from here next time.");
    } else {
        if (!built) {
            ImGui::TextWrapped("The game is built on this PC from your disc. This downloads about 1.4 GB of build tools "
                               "(Clang, CMake, Git and Microsoft's C++ libraries and Windows SDK) into the tools folder; "
                               "nothing is installed in Windows. The first build takes 30 to 90 minutes and needs about "
                               "15 GB free.");
            ImGui::TextDisabled("Free on this drive: %s", Megabytes(FreeBytes(app.root)).c_str());
            ImGui::Checkbox("I accept the license terms for Microsoft's C++ libraries and Windows SDK", &app.agreed);
            ImGui::SameLine();
            if (ImGui::SmallButton("Read them")) OpenUrl(kLicenseUrl);
        } else if (app.update) {
            std::string was = app.builder->BuiltVersion();
            if (IsReleaseVersion(app.game.version))
                ImGui::TextColored(ImVec4(0.4f, 0.75f, 1, 1), "Version %s of the game is ready to build (yours: %s).",
                                   app.game.version.c_str(), was.empty() ? "an earlier one" : was.c_str());
            else
                ImGui::TextColored(ImVec4(0.4f, 0.75f, 1, 1), "A new version is available.");
        } else {
            ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1), "Built and ready.");
        }
        std::string too_long;
        bool long_path = !built && app.builder->PathTooLong(too_long);  // built: the SDK is unpacked already
        if (long_path) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", too_long.c_str());
        bool can = app.copy.ok && (built || app.agreed) && !long_path;
        ImGui::BeginDisabled(!can);
        const char* label = !built ? "Build" : app.update ? "Update" : "Rebuild";
        if (ImGui::Button(label, ImVec2(10 * em, 0))) {
            if (!built) {
                std::ofstream(app.root / "agreed.txt") << kLicenseUrl;
            }
            app.update = false;
            app.music_job = false;
            app.Start([&app](std::string& e) { return app.builder->BuildAll(e); }, "The game is built. Press Play.");
        }
        ImGui::EndDisabled();
        if (app.music_job) {
        } else if (!error.empty()) {
            ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", error.c_str());
            if (ImGui::SmallButton("Open launcher.log")) OpenUrl(U8(app.root / "launcher.log"));
        } else if (!done.empty()) {
            ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1), "%s", done.c_str());
        }
    }

    Section("3  Play");
    ImGui::BeginDisabled(busy || !built || !app.copy.ok);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.55f, 0.25f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.65f, 0.3f, 1));
    ImGui::PushFont(big);
    if (ImGui::Button("Play", ImVec2(10 * em, 0))) {
        std::string e;
        if (!app.builder->Launch(e)) {
            std::lock_guard l(app.lock);
            app.error = e;
        }
    }
    ImGui::PopFont();
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::BeginDisabled(!built);
    if (ImGui::Button("Open the game's folder")) OpenFolder(app.builder->PlayDir());
    ImGui::EndDisabled();
    uint64_t build_bytes;
    {
        std::lock_guard l(app.lock);
        build_bytes = app.build_bytes;
    }
    ImGui::BeginDisabled(busy || !built || build_bytes == 0);
    std::string clean = "Remove the build files (" + Megabytes(build_bytes) + ")";
    if (ImGui::Button(clean.c_str()))
        app.music_job = false, app.Start([&app](std::string& e) { return app.builder->RemoveBuildFiles(e); },
                  "Build files removed. An update builds everything again.");
    ImGui::EndDisabled();
    ImGui::EndGroup();
    ImGui::TextDisabled("In the game: F1 opens the PC settings. Saves, settings and logs stay in the game's folder.");

    if (app.game.music) {
        Section("4  Your music");
        ImGui::TextWrapped("The game plays Xbox custom soundtracks. Choose a folder of your own music: each folder in it "
                           "becomes a soundtrack (MP3, FLAC, OGG, Opus, M4A, AAC, WAV, WMA, AIFF), and the songs in the "
                           "folder itself one more. They are converted for the game (ffmpeg, about 90 MB, downloaded "
                           "the first time; nothing is installed). In the game they are with the custom soundtracks.");
        if (!app.music.empty()) ImGui::TextDisabled("Your music: %s", U8(app.music).c_str());
        auto import = [&app](const fs::path& folder) {
            app.music_job = true;
            app.Start([&app, folder](std::string& e) {
                std::string summary;
                bool ok = app.builder->ImportMusic(folder, summary, e);
                std::lock_guard l(app.lock);
                if (ok) app.music = folder, app.music_summary = summary;
                return ok;
            }, "Your music is ready.");
        };
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Choose a music folder...")) {
            fs::path p = Pick(hwnd, true, L"Choose a folder of your music");
            if (!p.empty()) import(p);
        }
        if (!app.music.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Update my music")) import(app.music);
            ImGui::SameLine();
            ImGui::TextDisabled("after adding or changing songs: only those are converted");
        }
        ImGui::EndDisabled();
        if (app.music_job && !busy) {
            if (!error.empty()) {
                ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", error.c_str());
                if (ImGui::SmallButton("Open launcher.log##music")) OpenUrl(U8(app.root / "launcher.log"));
            } else if (!done.empty()) {
                std::string summary;
                {
                    std::lock_guard l(app.lock);
                    summary = app.music_summary;
                }
                ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1), "%s %s", done.c_str(), summary.c_str());
            }
        }
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Details")) {
        ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Border, ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::lock_guard l(app.lock);
            for (const std::string& line : app.log) ImGui::TextUnformatted(line.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - em) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
    ImGui::End();
}

void Style(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowPadding = ImVec2(24, 20);
    s.FramePadding = ImVec2(12, 7);
    s.ItemSpacing = ImVec2(10, 8);
    s.FrameRounding = 4;
    s.GrabRounding = 4;
    s.Colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.09f, 0.1f, 1);
    s.Colors[ImGuiCol_Button] = ImVec4(0.25f, 0.27f, 0.32f, 1);
    s.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.33f, 0.36f, 0.42f, 1);
    s.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.95f, 0.55f, 0.2f, 1);
    s.ScaleAllSizes(scale);
}

}  // namespace

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
    Style(scale);
    ImFont* body = nullptr;
    ImFont* big = nullptr;
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    std::string font = U8(fs::path(windir) / "Fonts" / "segoeui.ttf");
    if (fs::exists(fs::path(windir) / "Fonts" / "segoeui.ttf", ec)) {
        body = io.Fonts->AddFontFromFileTTF(font.c_str(), 17.0f * scale);
        big = io.Fonts->AddFontFromFileTTF(font.c_str(), 28.0f * scale);
    }
    if (!body) {
        ImFontConfig cfg;
        cfg.SizePixels = 13.0f * scale;
        body = io.Fonts->AddFontDefault(&cfg);
        big = body;
    }
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
        Draw(app, hwnd, big);
        ImGui::Render();
        const float clear[4] = {0.09f, 0.09f, 0.1f, 1};
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
