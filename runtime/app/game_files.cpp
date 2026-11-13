// The game in assets: its files, or a disc image (.iso) with this executable's xbe on it. The xbe must be the one
// this executable was recompiled from; another release's would run its own data under this code.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <SDL3/SDL.h>
#endif

#include <algorithm>
#include <cwctype>
#include <fstream>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"

extern const char kImageSha1[];  // the xbe recompiled into this executable, written by CMake

namespace app {

namespace {

namespace fs = std::filesystem;

// assets beside the exe or in any folder above it (the build output)
fs::path FindAssets() {
    auto dir = ExeDir();
    std::error_code ec;
    for (int i = 0; i < 6; i++) {
        if (fs::is_directory(dir / "assets", ec)) return dir / "assets";
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return {};
}

#ifndef _WIN32
std::string Utf8(const std::wstring& w) {
    std::string out;
    for (wchar_t c : w) {
        uint32_t u = uint32_t(c);
        if (u < 0x80) out += char(u);
        else if (u < 0x800) out += char(0xC0 | (u >> 6)), out += char(0x80 | (u & 0x3F));
        else out += char(0xE0 | (u >> 12)), out += char(0x80 | ((u >> 6) & 0x3F)), out += char(0x80 | (u & 0x3F));
    }
    return out;
}
#endif

void ShowError(const wchar_t* text) {
#ifdef _WIN32
    MessageBoxW(nullptr, text, kGame.short_title, MB_ICONERROR);
#else
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, Utf8(kGame.short_title).c_str(), Utf8(text).c_str(), nullptr);
#endif
}

bool AskYesNo(const std::wstring& text) {
#ifdef _WIN32
    return MessageBoxW(nullptr, text.c_str(), kGame.short_title, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES;
#else
    const SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "No"},
                                                {0, 1, "Yes"}};
    std::string title = Utf8(kGame.short_title), body = Utf8(text);
    const SDL_MessageBoxData box{SDL_MESSAGEBOX_WARNING, nullptr, title.c_str(), body.c_str(), 2, buttons, nullptr};
    int hit = 0;
    return SDL_ShowMessageBox(&box, &hit) && hit == 1;
#endif
}

bool ReadHostFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

}  // namespace

std::string Sha1(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

bool FindGame(GameFiles& game) {
    fs::path assets = FindAssets();
    std::vector<fs::path> isos;
    std::error_code ec;
    if (!assets.empty())
        for (auto& e : fs::directory_iterator(assets, ec)) {
            std::wstring ext = e.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
            if (ext == L".iso" && e.is_regular_file(ec)) isos.push_back(e.path());
        }
    std::sort(isos.begin(), isos.end());

    // the first copy with this xbe wins, the extracted files before any image; another release is kept to offer
    GameFiles other;
    std::unique_ptr<kernel::Disc> other_disc;
    fs::path other_name;
    auto consider = [&](const fs::path& name, std::vector<uint8_t> xbe, std::unique_ptr<kernel::Disc> disc) {
        std::string sha = Sha1(xbe);
        bool same = std::any_of(kGame.same_code.begin(), kGame.same_code.end(), [&](const char* c) { return sha == c; });
        bool match = sha == kImageSha1 || same;
        LOGR("Game: %s in %s, sha1 %s%s", IMAGE_XBE, name.string().c_str(), sha.c_str(),
             sha == kImageSha1 ? "" : same ? ", another release with the same code" : ", another release");
        if (match) {
            game = {assets, std::move(xbe)};
            kernel::MountDisc(std::move(disc));
            return true;
        }
        if (other_name.empty()) {
            other = {assets, std::move(xbe)};
            other_disc = std::move(disc);
            other_name = name;
        }
        return false;
    };

    // the extracted files: the xbe with the disc's folders beside it. The xbe alone beside a disc image is the copy
    // the build recompiles from (README), not the game's files
    bool folders = false;
    if (!assets.empty())
        for (auto& e : fs::directory_iterator(assets, ec)) folders |= e.is_directory(ec);
    std::vector<uint8_t> xbe;
    if (!assets.empty() && (folders || isos.empty()) && ReadHostFile(assets / IMAGE_XBE, xbe) &&
        consider(assets, std::move(xbe), nullptr))
        return true;
    for (const fs::path& iso : isos) {
        auto disc = std::make_unique<kernel::Disc>();
        std::string error;
        if (!disc->Open(iso, error)) {
            LOGW("game: %s: %s", iso.string().c_str(), error.c_str());
            continue;
        }
        if (!disc->Load(IMAGE_XBE, xbe)) {
            LOGW("game: %s has no %s", iso.string().c_str(), IMAGE_XBE);
            continue;
        }
        if (consider(iso, std::move(xbe), std::move(disc))) return true;
    }

    if (other_name.empty()) {
        ShowError(kGame.missing);
        return false;
    }
    std::wstring text = other_name.filename().wstring() +
                        L" is a different release of the game than this build was made from, and will most likely "
                        L"crash.\n\nStart it anyway?";
    if (!AskYesNo(text)) return false;
    game = std::move(other);
    kernel::MountDisc(std::move(other_disc));
    return true;
}

}  // namespace app
