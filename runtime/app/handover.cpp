// A title may relaunch itself between the front end and a level (a quick reboot on the Xbox, a new process here).
// The old instance keeps its window up in kernel::Relaunch until this one has presented its first frames, so the
// kept picture stays on screen through the switch. Here the window is created hidden and opens at that point.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cwchar>

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace app {

namespace {
HANDLE g_ready = nullptr;
std::atomic<bool> g_handing{false};
}  // namespace

// the previous instance's id is on the command line (--handover=pid), its event named by it
bool StartHandover() {
    const wchar_t* arg = std::wcsstr(GetCommandLineW(), L"--handover=");
    if (!arg) return false;
    unsigned long previous = std::wcstoul(arg + 11, nullptr, 10);
    g_ready = previous ? OpenEventW(EVENT_MODIFY_STATE, FALSE, kernel::HandoverEvent(previous).c_str()) : nullptr;
    g_handing = g_ready != nullptr;
    if (g_handing) LOGI("handover: the previous instance waits for the first frames");
    return g_handing;
}

bool HandingOver() { return g_handing; }

// each present from the second on: the window opens, and the old instance goes only some frames later. A window just shown has
// nothing on screen until its next present, so going at once showed the desktop for a frame (AUF, leaving a driving mission)
void FinishHandover() {
    static int shown = -1;
    if (!g_handing) return;
    if (shown < 0) {
        OpenGameWindow(false);
        shown = 0;
        return;
    }
    if (++shown < 3) return;
    g_handing = false;
    SetEvent(g_ready);
    CloseHandle(g_ready);
}

}  // namespace app
