#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <fstream>
#include <vector>

#include "screen.h"
#include "runtime/app/classic_theme.h"

namespace launcher {

using namespace classic;

namespace {

constexpr const char* kLicenseUrl = "https://go.microsoft.com/fwlink/?LinkId=2179911";  // Visual Studio Build Tools' terms
constexpr float kBodyFont = 15;

enum Page { kGamePage, kMusicPage, kManagePage, kLogPage };

void TextIn(ImU32 colour, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

void Section(const char* title, ImFont* heading) {
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.5f));
    ImGui::PushFont(heading);
    ImGui::TextColored(Col(kAccent), "%s", title);
    ImGui::PopFont();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(pos, ImVec2(pos.x + w, pos.y), kDark);
    dl->AddLine(ImVec2(pos.x, pos.y + 1), ImVec2(pos.x + w, pos.y + 1), kLight & 0x60ffffff);
    ImGui::Dummy(ImVec2(0, 4));
}

// the primary action of a page: the selection's mustard
bool Primary(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, kSelect);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccent);
    ImGui::PushStyleColor(ImGuiCol_Text, kWhite);
    bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return pressed;
}

void Results(App& app, const std::string& error, const std::string& done, const char* log_id) {
    if (!error.empty()) {
        TextIn(kBad, error);
        if (ImGui::SmallButton(log_id)) OpenUrl(U8(app.root / "launcher.log"));
    } else if (!done.empty()) {
        TextIn(kGood, done);
    }
}

enum class Mark { Waiting, Working, Done };

// one step of the install: a tick when done, a pulsing box while it runs, an empty box to come; what it is and how far
void StepRow(Mark mark, const char* name, const std::string& detail, const Status* live) {
    const float em = ImGui::GetFontSize();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = em * 2;
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (mark == Mark::Working) dl->AddRectFilled(pos, pos + ImVec2(w, h), kHover);
    ImVec2 box = pos + ImVec2(em * 0.45f, em * 0.5f), end = box + ImVec2(em, em);
    dl->AddRectFilled(box, end, kField);
    dl->AddRect(box, end, kDark);
    if (mark == Mark::Done) {
        dl->AddLine(box + ImVec2(em * 0.2f, em * 0.5f), box + ImVec2(em * 0.42f, em * 0.75f), kGood, 2);
        dl->AddLine(box + ImVec2(em * 0.42f, em * 0.75f), box + ImVec2(em * 0.8f, em * 0.25f), kGood, 2);
    } else if (mark == Mark::Working) {
        float pulse = 0.55f + 0.45f * std::sin(float(ImGui::GetTime()) * 4);
        ImU32 fill = (kAccent & 0x00ffffff) | (ImU32(pulse * 255) << 24);
        dl->AddRectFilled(box + ImVec2(em * 0.22f, em * 0.22f), end - ImVec2(em * 0.22f, em * 0.22f), fill);
    }
    ImVec2 text = pos + ImVec2(em * 2, (h - em) * 0.5f);
    dl->AddText(text, mark == Mark::Waiting ? kDim : mark == Mark::Working ? kWhite : kText, name);
    std::string right = live && !live->detail.empty() ? live->detail : detail;
    ImVec2 size = ImGui::CalcTextSize(right.c_str());
    dl->AddText(ImVec2(pos.x + w - size.x - em * 0.6f, text.y), mark == Mark::Working ? kAccent : kDim, right.c_str());
}

// a prebuilt install: the steps, then the action
void InstallSection(App& app, ImFont* heading, const Status& st, const std::string& error, const std::string& done) {
    const float em = ImGui::GetFontSize();
    const bool busy = app.busy && !app.music_job, built = app.builder->Built();
    Step at = app.builder->step;
    if (built && !busy && !app.update) at = Step::Ready;
    auto mark = [&](Step s) { return at == s && busy ? Mark::Working : int(at) > int(s) ? Mark::Done : Mark::Waiting; };
    const bool legacy = !built && app.builder->Legacy();
    Section(built && !app.update ? "Ready to play" : legacy ? "A faster way to play" : "Getting the game ready", heading);
    if (legacy && !busy) {
        ImGui::TextWrapped("This version comes ready-made: your PC only builds the game's own code, in a minute or two, "
                           "with no build tools to download. Your saves, settings and music are kept, and the game you have "
                           "keeps working until the new one is ready.");
        ImGui::Spacing();
    } else if (!built) {
        ImGui::TextWrapped("The program comes ready to go, and your PC builds the game's code from your own disc, once. "
                           "Nothing is installed in Windows, and it all stays in one folder.");
        ImGui::Spacing();
    }
    StepRow(app.copy.ok ? Mark::Done : Mark::Waiting, "Your game", app.copy.ok ? app.copy.release : "not chosen yet", nullptr);
    StepRow(mark(Step::Package), "Download the program", "", at == Step::Package && busy ? &st : nullptr);
    StepRow(mark(Step::Unpack), "Unpack it", "", at == Step::Unpack && busy ? &st : nullptr);
    StepRow(mark(Step::Compile), "Build the game for this PC", "", at == Step::Compile && busy ? &st : nullptr);
    ImGui::Spacing();
    if (busy) {
        TextIn(kDim, "You can stop at any time: it carries on from there next time. Your saves and music are not touched.");
        return;
    }
    if (app.update) {
        std::string was = app.builder->BuiltVersion();
        TextIn(kAccent, IsReleaseVersion(app.game.version)
                            ? "Version " + app.game.version + " is ready to install (yours: " + (was.empty() ? std::string("an earlier one") : was) + "). Your saves, settings and music are kept."
                            : std::string("A new version is available."));
    }
    if (!built || app.update) {
        TextIn(kDim, "Free on this drive: " + Megabytes(FreeBytes(app.root)));
        ImGui::BeginDisabled(!app.copy.ok);
        if (Primary(built || legacy ? "Update" : "Get the game ready", ImVec2(14 * em, 0))) {
            app.update = false;
            app.music_job = false;
            app.Start([&app](std::string& e) { return app.builder->BuildAll(e); }, "The game is ready. Press Play.");
        }
        ImGui::EndDisabled();
    }
    if (!app.music_job) Results(app, error, done, "Open launcher.log");
}

void GamePage(App& app, void* window, ImFont* heading, const Status& st, const std::string& error, const std::string& done) {
    const float em = ImGui::GetFontSize();
    const bool busy = app.busy, built = app.builder->Built();

    if (app.launcher_update && !app.later) {
        Release r;
        {
            std::lock_guard l(app.release_lock);
            r = app.release;
        }
        Section("A new version", heading);
        TextIn(kAccent, "Version " + r.tag + " is out (this launcher is " + app.game.version + ").");
        ImGui::TextWrapped("Update the launcher, then build the game again with the new version. Your saves and settings are kept.");
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

    Section("Your game", heading);
    if (app.copy.ok) {
        TextIn(kGood, app.copy.release);
        TextIn(kDim, U8(app.copy.path));
    } else {
        ImGui::TextWrapped("Choose your copy of the game: a disc image (.iso) of your own disc, or a folder with the "
                           "disc's files. Nothing is copied off the internet: the game comes from your disc.");
        if (!app.copy.error.empty()) TextIn(kBad, app.copy.error);
    }
    ImGui::BeginDisabled(busy);
    for (int folder = 0; folder < 2; folder++) {
        if (folder) ImGui::SameLine();
        if (ImGui::Button(folder ? "Choose a folder..." : "Choose a disc image...")) {
            fs::path p = Pick(window, folder != 0);
            if (!p.empty()) {
                app.copy = CheckGame(app.game, p);
                std::string e;
                if (app.copy.ok && !app.builder->SetGame(app.copy, e)) app.copy.ok = false, app.copy.error = e;
            }
        }
    }
    ImGui::EndDisabled();

    if (app.builder->Prebuilt()) {
        InstallSection(app, heading, st, error, done);
    } else {
    Section("Build", heading);
    if (busy) {
        TextIn(kDim, "Working: see the bar below. It carries on from there next time if you stop it.");
    } else {
        if (!built) {
            ImGui::TextWrapped("The game is built on this PC from your disc. This downloads about 1.4 GB of build tools "
                               "(Clang, CMake, Git and Microsoft's C++ libraries and Windows SDK) into the tools folder; "
                               "nothing is installed in Windows. The first build takes 30 to 90 minutes and needs about "
                               "3 GB free, plus room for your copy of the game.");
            TextIn(kDim, "Free on this drive: " + Megabytes(FreeBytes(app.root)));
            ImGui::Checkbox("I accept the license terms for Microsoft's C++ libraries and Windows SDK", &app.agreed);
            ImGui::SameLine();
            if (ImGui::SmallButton("Read them")) OpenUrl(kLicenseUrl);
        } else if (app.update) {
            std::string was = app.builder->BuiltVersion();
            if (IsReleaseVersion(app.game.version))
                TextIn(kAccent, "Version " + app.game.version + " of the game is ready to build (yours: " +
                                    (was.empty() ? std::string("an earlier one") : was) + ").");
            else
                TextIn(kAccent, "A new version is available.");
        } else {
            TextIn(kGood, "Built and ready.");
        }
        std::string too_long;
        bool long_path = !built && app.builder->PathTooLong(too_long);  // built: the SDK is unpacked already
        if (long_path) TextIn(kBad, too_long);
        bool can = app.copy.ok && (built || app.agreed) && !long_path;
        ImGui::BeginDisabled(!can);
        const char* label = !built ? "Build" : app.update ? "Update" : "Rebuild";
        const bool first = !built || app.update;
        if (first ? Primary(label, ImVec2(10 * em, 0)) : ImGui::Button(label, ImVec2(10 * em, 0))) {
            if (!built) std::ofstream(app.root / "agreed.txt") << kLicenseUrl;
            app.update = false;
            app.music_job = false;
            app.Start([&app](std::string& e) { return app.builder->BuildAll(e); }, "The game is built. Press Play.");
        }
        ImGui::EndDisabled();
        if (!app.music_job) Results(app, error, done, "Open launcher.log");
    }
    }

    Section("Play", heading);
    // a game built the old way plays as it is until its update is in place
    ImGui::BeginDisabled(busy || !(built || app.builder->Legacy()) || !app.copy.ok);
    ImGui::PushFont(heading);
    if (Primary("Play", ImVec2(10 * em, 0))) {
        std::string e;
        if (!app.builder->Launch(e)) {
            std::lock_guard l(app.lock);
            app.error = e;
        }
    }
    ImGui::PopFont();
    ImGui::EndDisabled();
    TextIn(kDim, "In the game: F1 opens the PC settings. Saves, settings and logs stay in the game's folder.");
}

void MusicPage(App& app, void* window, ImFont* heading, const std::string& error, const std::string& done) {
    const bool busy = app.busy;
    Section("Your music", heading);
    ImGui::TextWrapped("The game plays Xbox custom soundtracks. Choose a folder of your own music: each folder in it "
                       "becomes a soundtrack (MP3, FLAC, OGG, Opus, M4A, AAC, WAV, WMA, AIFF), and the songs in the "
                       "folder itself one more. They are converted for the game (ffmpeg, about 90 MB, downloaded "
                       "the first time; nothing is installed). In the game they are with the custom soundtracks.");
    if (!app.music.empty()) TextIn(kDim, "Your music: " + U8(app.music));
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
        fs::path p = Pick(window, true, L"Choose a folder of your music");
        if (!p.empty()) import(p);
    }
    if (!app.music.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Update my music")) import(app.music);
        TextIn(kDim, "After adding or changing songs: only those are converted.");
    }
    ImGui::EndDisabled();
    if (app.music_job && !busy) {
        std::string summary;
        {
            std::lock_guard l(app.lock);
            summary = app.music_summary;
        }
        Results(app, error, done.empty() ? done : done + " " + summary, "Open launcher.log##music");
    }
}

void ManagePage(App& app, ImFont* heading) {
    const bool busy = app.busy, built = app.builder->Built();
    const float em = ImGui::GetFontSize();
    Section("This install", heading);
    if (ImGui::BeginTable("install", 2)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, em * 10);
        auto row = [](const char* name, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            TextIn(kDim, name);
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", value.c_str());
        };
        row("Launcher", app.game.version);
        std::string version = app.builder->BuiltVersion();
        row("Game", app.builder->Legacy() ? (version.empty() ? std::string("an earlier version") : version) + ", built the old way: update it on Game"
                    : !built ? "not ready yet" : version.empty() ? "ready" : version);
        row("Your copy", app.copy.ok ? app.copy.release + ", " + U8(app.copy.path) : "not chosen");
        row("Folder", U8(app.root));
        row("Free on this drive", Megabytes(FreeBytes(app.root)));
        ImGui::EndTable();
    }
    Section("Folders", heading);
    ImGui::BeginDisabled(!built);
    if (ImGui::Button("Open the game's folder")) OpenFolder(app.builder->PlayDir());
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open launcher.log")) OpenUrl(U8(app.root / "launcher.log"));
    Section("Space and repairs", heading);
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
    TextIn(kDim, "Frees space once the game is built. Your saves, settings and music are not touched.");
    if (app.builder->Prebuilt()) {
        uint64_t old_bytes;
        {
            std::lock_guard l(app.lock);
            old_bytes = app.old_bytes;
        }
        if (old_bytes > 0) {
            ImGui::BeginDisabled(busy || !built);
            std::string old = "Remove the old build tools (" + Megabytes(old_bytes) + ")";
            if (ImGui::Button(old.c_str()))
                app.music_job = false, app.Start([&app](std::string& e) { return app.builder->RemoveOldBuild(e); },
                          "The old build tools are removed.");
            ImGui::EndDisabled();
            TextIn(kDim, built ? "The earlier versions built the game with these; this one does not need them. FFmpeg, for your music, stays."
                               : "Once the game is updated: the earlier versions built it with these.");
        }
    }
    if (app.builder->Prebuilt()) {
        ImGui::BeginDisabled(busy || !built || !app.copy.ok);
        if (ImGui::Button("Build the game again")) {
            app.music_job = false;
            app.Start([&app](std::string& e) {
                std::error_code ec;
                fs::remove(app.builder->PlayDir() / (app.game.exe + ".dll"), ec);
                return app.builder->BuildAll(e);
            }, "The game is ready. Press Play.");
        }
        ImGui::EndDisabled();
        TextIn(kDim, "If the game will not start. Takes a minute or two, and keeps your saves.");
    }
}

void LogPage(App& app) {
    const float em = ImGui::GetFontSize();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kField);
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Border, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard l(app.lock);
        for (const std::string& line : app.log) ImGui::TextUnformatted(line.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - em) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

}  // namespace

void ScreenStyle(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(0, 0);
    s.FramePadding = ImVec2(10, 5);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.ScrollbarSize = 14;
    s.GrabMinSize = 12;
    classic::Apply(s);
    s.ScaleAllSizes(scale);
}

ImFont* ScreenFonts(const fs::path& folder, float scale) {
    // Tahoma, as classic Steam and the game's settings window; ImGui's own without it
    ImGuiIO& io = ImGui::GetIO();
    std::error_code ec;
    ImFont* heading = nullptr;
    if (fs::exists(folder / "tahoma.ttf", ec)) io.Fonts->AddFontFromFileTTF(U8(folder / "tahoma.ttf").c_str(), kBodyFont * scale);
    if (io.Fonts->Fonts.empty()) {
        ImFontConfig cfg;
        cfg.SizePixels = 13.0f * scale;
        io.Fonts->AddFontDefault(&cfg);
    }
    if (fs::exists(folder / "tahomabd.ttf", ec)) heading = io.Fonts->AddFontFromFileTTF(U8(folder / "tahomabd.ttf").c_str(), kBodyFont * scale);
    return heading ? heading : io.Fonts->Fonts[0];
}

void DrawScreen(App& app, void* window, ImFont* heading) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    const float em = ImGui::GetFontSize(), pad = em * 0.8f;
    const ImVec2 origin = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    const float head_h = em * 2.4f, side_w = em * 9.5f, foot_h = em * 4.4f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    std::string error, done;
    Status st;
    {
        std::lock_guard l(app.lock);
        error = app.error;
        done = app.done_message;
        st = app.status;
    }
    const bool busy = app.busy;

    // the header: the game's name and the launcher's version
    dl->AddRectFilled(origin, origin + ImVec2(size.x, head_h), kHead);
    dl->AddLine(origin + ImVec2(0, head_h), origin + ImVec2(size.x, head_h), kDark);
    ImGui::PushFont(heading);
    dl->AddText(origin + ImVec2(pad, (head_h - em) * 0.5f), kWhite, app.game.title.c_str());
    ImGui::PopFont();
    std::string version = "Launcher " + app.game.version;
    dl->AddText(origin + ImVec2(size.x - pad - ImGui::CalcTextSize(version.c_str()).x, (head_h - em) * 0.5f), kDim, version.c_str());

    // the sections
    struct Entry {
        const char* name;
        int page;
    };
    std::vector<Entry> entries = {{"Game", kGamePage}};
    if (app.game.music) entries.push_back({"Your music", kMusicPage});
    entries.push_back({"Manage", kManagePage});
    entries.push_back({"Log", kLogPage});
    ImGui::SetCursorScreenPos(origin + ImVec2(0, head_h + 1));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kSide);
    ImGui::BeginChild("##sections", ImVec2(side_w, size.y - head_h - 1), 0, ImGuiWindowFlags_NoScrollbar);
    ImGui::Dummy(ImVec2(0, em * 0.3f));
    for (const Entry& e : entries) {
        ImVec2 pos = ImGui::GetCursorScreenPos(), row(side_w, em * 1.9f);
        if (ImGui::InvisibleButton(e.name, row)) app.page = e.page;
        ImDrawList* side = ImGui::GetWindowDrawList();
        const bool on = app.page == e.page;
        if (on) side->AddRectFilled(pos, pos + row, kSelect);
        else if (ImGui::IsItemHovered()) side->AddRectFilled(pos, pos + row, kHover);
        side->AddText(pos + ImVec2(pad, (row.y - em) * 0.5f), on ? kWhite : kText, e.name);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // the page
    ImGui::SetCursorScreenPos(origin + ImVec2(side_w, head_h + 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad * 1.5f, pad));
    ImGui::BeginChild("##page", ImVec2(size.x - side_w, size.y - head_h - 1 - foot_h), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    if (app.page == kMusicPage && app.game.music) MusicPage(app, window, heading, error, done);
    else if (app.page == kLogPage) LogPage(app);
    else if (app.page == kManagePage) ManagePage(app, heading);
    else GamePage(app, window, heading, st, error, done);
    ImGui::EndChild();

    // the footer: the work going on, on every page; else where everything goes
    const ImVec2 foot = origin + ImVec2(side_w, size.y - foot_h);
    dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(foot, origin + size, kHead);
    dl->AddLine(foot, foot + ImVec2(size.x - side_w, 0), kDark);
    ImGui::SetCursorScreenPos(foot + ImVec2(pad * 1.5f, em * 0.5f));
    ImGui::PushItemWidth(size.x - side_w - pad * 3);
    if (busy) {
        ImGui::TextUnformatted(st.step.empty() ? "Starting..." : st.step.c_str());
        ImGui::SetCursorScreenPos(ImVec2(foot.x + pad * 1.5f, ImGui::GetCursorScreenPos().y));
        const float stop_w = em * 5;
        const float bar_w = size.x - side_w - pad * 3 - stop_w - ImGui::GetStyle().ItemSpacing.x;
        const char* detail = st.detail.empty() ? "" : st.detail.c_str();
        ImGui::ProgressBar(st.fraction >= 0 ? float(st.fraction) : -float(ImGui::GetTime()), ImVec2(bar_w, 0), detail);
        ImGui::SameLine();
        if (ImGui::Button("Stop", ImVec2(stop_w, 0))) app.builder->cancel = true;
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted("PC port launcher. Everything goes in");
        ImGui::SetCursorScreenPos(ImVec2(foot.x + pad * 1.5f, ImGui::GetCursorScreenPos().y));
        ImGui::TextUnformatted(U8(app.root).c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopItemWidth();
    ImGui::End();
}

}  // namespace launcher
