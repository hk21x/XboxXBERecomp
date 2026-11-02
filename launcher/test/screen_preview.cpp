// The launcher's screen with no window or GPU, each state as a PNG: screen_preview <game.json> <out dir> <font folder>
#include <cstdio>
#include <fstream>

#include "screen.h"
#include "../tools/ui_preview/raster.h"

namespace launcher {
fs::path Pick(void*, bool, const wchar_t*) { return {}; }
void OpenUrl(const std::string&) {}
void OpenFolder(const fs::path&) {}
uint64_t FreeBytes(const fs::path&) { return 482ull << 30; }
bool UpdateLauncher(App&, const Release&, std::string&) { return true; }
}  // namespace launcher
using namespace launcher;

static void Shot(App& app, ImFont* big, const std::string& path, int w, int h) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(w), float(h));
    Image im{w, h, std::vector<uint8_t>(size_t(w * h) * 3)};
    for (int i = 0; i < 3; i++) {
        io.DeltaTime = 1 / 60.0f;
        ImGui::NewFrame();
        DrawScreen(app, nullptr, big);
        ImGui::Render();
    }
    Draw(im);
    Png(im, path);
}

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    std::ifstream in(argv[1]);
    std::string text((std::istreambuf_iterator<char>(in)), {}), e, out = argv[2];
    fs::create_directories(out);
    App app;
    if (!ParseGameConfig(text, app.game, e)) return std::puts(e.c_str()), 2;
    app.root = fs::temp_directory_path() / "screen_preview_root";
    std::error_code ec;
    fs::remove_all(app.root, ec);
    fs::create_directories(app.root, ec);
    app.builder = std::make_unique<Builder>(app.game, app.root, *app.http, *app.platform);
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ScreenStyle(1);
    ImFont* big = ScreenFonts(argv[3], 1);
    ImGui::GetIO().Fonts->Build();
    const int W = 900, H = 700;
    Shot(app, big, out + "/1_first_run.png", W, H);
    app.copy.ok = true, app.copy.release = "NTSC (USA)", app.copy.path = "C:\\Games\\PGR2\\PGR2_USA.iso", app.agreed = true;
    app.busy = true;
    app.status = {"Building " + app.game.title, "38 of 65 steps", 0.58};
    Shot(app, big, out + "/2_building.png", W, H);
    app.busy = false, app.status = {};
    fs::create_directories(app.builder->PlayDir(), ec);
    std::ofstream(app.builder->GameExe()) << "x";
    app.build_bytes = 231ull << 20;
    app.done_message = "The game is built. Press Play.";
    Shot(app, big, out + "/3_built.png", W, H);
    app.page = 1, app.music = "D:\\Music", app.music_job = true, app.music_summary = "3 soundtracks, 41 songs.", app.done_message = "Your music is ready.";
    Shot(app, big, out + "/4_music.png", W, H);
    app.page = 2;
    for (int i = 0; i < 30; i++) app.log.push_back("09:14:" + std::to_string(10 + i) + " [" + std::to_string(i * 10) + "/434] Building CXX object");
    Shot(app, big, out + "/5_log.png", W, H);
    app.page = 0, app.music_job = false, app.done_message.clear(), app.error = "The build failed, see launcher.log", app.launcher_update = true, app.release.tag = "v1.6.0";
    Shot(app, big, out + "/6_update_and_error.png", W, H);
    fs::remove_all(app.root, ec);
    std::puts("SCREEN PREVIEW OK");
    return 0;
}
