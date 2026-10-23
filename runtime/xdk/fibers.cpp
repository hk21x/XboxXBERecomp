// XAPI fibers on host coroutines. SwitchToFiber swaps the guest stack in assembly, which recompiled code cannot
// follow: each fiber runs on a host stack of its own (Windows fibers, ucontext elsewhere) with guest registers of its
// own, and a switch moves the host thread between them. XAPI's CreateFiber and ConvertThreadToFiber keep their
// console code: they only build the guest records (5849, Forza 0x268A91 and 0x268B6F). A record is
// +0 FiberData, +4 StackBase, +8 StackLimit, +0xC the saved stack pointer; a new fiber's start routine is at
// StackBase - 0x14. The thread's current fiber is at its TLS block + 0xC.
#include <memory>
#include <unordered_map>

#include "core/log.h"
#include "xbe/xbe.h"
#include "xdk/hle.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <ucontext.h>
#endif

using namespace kernel;

namespace kernel {
extern const xbe::Image* g_image;
}

// configs written before the SDK knew the name
#ifndef SYM_SwitchToFiber
#define SYM_SwitchToFiber FFFFFD00
#define HAS_SwitchToFiber 0
#endif
#ifndef SYM_DeleteFiber
#define SYM_DeleteFiber FFFFFD01
#define HAS_DeleteFiber 0
#endif

namespace {

constexpr size_t kHostStack = 16u << 20;  // recompiled code nests host frames as deep as the guest's calls

struct HostFiber {
    X86Context ctx;               // a fiber's own registers; the thread's first fiber uses the thread's context
    X86Context* active = nullptr;  // the context its frames use
    uint32_t record = 0;
    uint32_t exception_list = 0xFFFFFFFF;  // fs:[0] while it is switched out
    bool started = false;
#ifdef _WIN32
    void* fiber = nullptr;
#else
    ucontext_t uc{};
    void* stack = nullptr;
#endif
};

// fibers run on the thread that made them current
thread_local std::unordered_map<uint32_t, std::unique_ptr<HostFiber>> t_fibers;

// the thread's TLS block, found as XAPI finds it: [StackBase + tls_index * 4]
uint32_t TlsBlock(const X86Context& ctx) {
    uint32_t index = g_image && g_image->tls.index_addr ? R32(g_image->tls.index_addr) : 0;
    return R32(R32(ctx.fs_base + 4) + index * 4);
}

// a new fiber's first frame: its start routine with FiberData, then the thread ends, as XAPI's start thunk does
// (Forza 0x268A4C)
[[noreturn]] void Run(HostFiber* f) {
    uint32_t start = R32(R32(f->record + 4) - 0x14);
    LOGI("fibers: fiber %08X starts at %08X (%08X)", f->record, start, R32(f->record));
    CallGuest(f->ctx, start, {R32(f->record)});
    LOGI("fibers: fiber %08X returned, its thread exits", f->record);
    ExitThread(0);
}

#ifdef _WIN32
void WINAPI Entry(void* p) { Run(static_cast<HostFiber*>(p)); }
#else
// makecontext passes ints: the fiber's address in two halves
void Entry(unsigned lo, unsigned hi) { Run(reinterpret_cast<HostFiber*>(uintptr_t(lo) | uintptr_t(hi) << 32)); }
#endif

void Release(HostFiber& f) {
#ifdef _WIN32
    if (f.fiber && f.active == &f.ctx) ::DeleteFiber(f.fiber);  // made here, not the thread's own
#else
    if (f.stack) munmap(f.stack, kHostStack);
#endif
}

}  // namespace

// (fiber): the host thread moves to the fiber's host stack; back here when another fiber switches to this one
HLE_STDCALL(SwitchToFiber, 1) {
    uint32_t tls = TlsBlock(ctx), from = R32(tls + 0xC), to = a[0];
    if (!to || to == from) return 0;
    auto& cur = t_fibers[from];
    if (!cur) {  // the thread's own fiber (ConvertThreadToFiber), running on the thread's host stack
        cur = std::make_unique<HostFiber>();
        cur->record = from;
        cur->started = true;
        cur->active = &ctx;
#ifdef _WIN32
        cur->fiber = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
#endif
    }
    auto& next = t_fibers[to];
    if (!next) {
        next = std::make_unique<HostFiber>();
        next->record = to;
        next->ctx = ctx;
        next->ctx.esp = (to - 0x40) & ~15u;  // below the record, as the console's first frame is
        next->active = &next->ctx;
#ifdef _WIN32
        next->fiber = CreateFiberEx(64u << 10, kHostStack, FIBER_FLAG_FLOAT_SWITCH, Entry, next.get());
#else
        next->stack = mmap(nullptr, kHostStack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK,
                           -1, 0);
        getcontext(&next->uc);
        next->uc.uc_stack.ss_sp = next->stack;
        next->uc.uc_stack.ss_size = kHostStack;
        next->uc.uc_link = nullptr;
        uintptr_t p = reinterpret_cast<uintptr_t>(next.get());
        makecontext(&next->uc, reinterpret_cast<void (*)()>(Entry), 2, unsigned(p), unsigned(p >> 32));
#endif
    }
    HostFiber* out = cur.get();
    HostFiber* in = next.get();
    // as XAPI's switch: the current fiber recorded in the TLS block, each fiber's own exception list in fs:[0]
    out->exception_list = R32(ctx.fs_base);
    W32(tls + 0xC, to);
    W32(ctx.fs_base, in->exception_list);
    Current()->ctx = in->active;
    in->started = true;
#ifdef _WIN32
    ::SwitchToFiber(in->fiber);
#else
    swapcontext(&out->uc, &in->uc);
#endif
    // switched back to
    Current()->ctx = out->active;
    W32(ctx.fs_base, out->exception_list);
    return 0;
}

// (fiber): its host stack goes with its guest stack (MmDeleteKernelStack from StackBase, as XAPI's DeleteFiber)
HLE_STDCALL(DeleteFiber, 1) {
    uint32_t f = a[0];
    if (!f) return 0;
    auto it = t_fibers.find(f);
    if (it != t_fibers.end() && it->second->active != &ctx) {
        Release(*it->second);
        t_fibers.erase(it);
    }
    mem::FreeStack(R32(f + 4));
    return 0;
}
