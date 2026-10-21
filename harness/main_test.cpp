// headless host: <exe> <game folder or disc image> [--instance N], run from the folder that gets logs, hdd and cap
// the game folder may come from GAME_DIR instead
#include <chrono>
#include <csignal>
#include <execinfo.h>
#include <ucontext.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "kernel/disc.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

// a crash: the faulting address, the host instruction and a backtrace on stderr (run.out); addr2line -e <exe> turns
// the addresses into functions (recompiled guest code is __imp__sub_<guest address>)
void OnCrash(int sig, siginfo_t* info, void* uc) {
    char line[160];
    auto* ctx = static_cast<ucontext_t*>(uc);
    int n = std::snprintf(line, sizeof line, "crash: signal %d at %p, rip %llx, guest esp (rdi+16) %08x\n", sig, info->si_addr,
                          (unsigned long long)ctx->uc_mcontext.gregs[REG_RIP],
                          ctx->uc_mcontext.gregs[REG_RDI] ? *reinterpret_cast<const uint32_t*>(ctx->uc_mcontext.gregs[REG_RDI] + 16) : 0u);
    if (n > 0) (void)!write(2, line, size_t(n));
    void* frames[32];
    backtrace_symbols_fd(frames, backtrace(frames, 32), 2);
    logx::Flush();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

int main(int argc, char** argv) {
    struct sigaction sa{};
    sa.sa_sigaction = OnCrash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    std::string game;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--instance") && i + 1 < argc) setenv("INSTANCE", argv[++i], 1);
        else game = argv[i];
    }
    // a relaunch into another image comes back without arguments
    if (game.empty() && std::getenv("GAME_DIR")) game = std::getenv("GAME_DIR");
    setenv("GAME_DIR", game.c_str(), 1);
    if (game.empty()) {
        std::fprintf(stderr, "usage: %s <game folder> [--instance N]\n", argv[0]);
        return 2;
    }
    logx::Init((app::ExeDir() / "logs").string(), app::kGame.log);
    app::Load();
    if (app::kGame.init) app::kGame.init();
    if (app::kGame.window) app::kGame.window(nullptr);
    std::string hdd = app::Instance() > 1 ? "hdd" + std::to_string(app::Instance()) : "hdd";
    std::vector<uint8_t> xbe;
    if (std::filesystem::is_regular_file(game)) {
        // a disc image: mounted as the DVD, the xbe read from it, the folder beside it stands for the game folder
        auto disc = std::make_unique<kernel::Disc>();
        std::string error;
        if (!disc->Open(game, error) || !disc->Load(IMAGE_XBE, xbe)) {
            std::fprintf(stderr, "%s: %s\n", game.c_str(), error.empty() ? "no " IMAGE_XBE : error.c_str());
            return 1;
        }
        kernel::MountDisc(std::move(disc));
        game = std::filesystem::path(game).parent_path().string();
    } else {
        std::ifstream f(game + "/" + IMAGE_XBE, std::ios::binary);
        xbe.assign(std::istreambuf_iterator<char>(f), {});
    }
    if (!kernel::Boot(std::move(xbe), IMAGE_XBE, game, (app::ExeDir() / hdd).string())) return 1;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        logx::Flush();
    }
}
