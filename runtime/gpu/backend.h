#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include "gpu/state.h"

namespace gpu {

// D3DCLEAR flags as the XDK defines them
enum : uint32_t {
    kClearTarget = 0xF0,  // ARGB channels
    kClearZ = 0x01,
    kClearStencil = 0x02,
};

struct Assembled;

class Backend {
public:
    virtual ~Backend() = default;
    // the title's back buffer size, the output window may differ
    virtual bool Init(uint32_t width, uint32_t height) = 0;
    // rects: count D3DRECTs (4 x int32), null for the whole viewport
    virtual void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                       uint32_t stencil) = 0;
    // indices: the title's 16 bit indices, null for non indexed draws, then start is the first vertex
    virtual void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) = 0;
    // the same draw, already assembled from this state and indices on another thread (the render thread's prep
    // thread): pre and Assemble's answer ok, which the backend may use in place of assembling it; read only during the
    // call
    // The pipeline (docs/render-parallel.md): the backend's draws are decided on the render thread and recorded on the
    // render thread's record thread. true: the backend does so. drain, called (on the render thread) before the
    // backend writes its list or record's state itself, returns once record has recorded every draw handed so far.
    // RecordNext (the record thread) records the next draw, its state s as it was for it; once per Draw, in order
    virtual bool RecordOnThread(std::function<void()> /*drain*/) { return false; }
    virtual void RecordNext(const State& /*s*/) {}
    // clears too are decided and handed to record (a RecordNext each), not fences
    virtual bool ClearsRecorded() const { return false; }
    // ClearReport and GetReport are called on the record thread, in order with its draws, not as fences
    virtual bool ReportsRecorded() const { return false; }
    virtual void DrawAssembled(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices,
                               const Assembled& /*pre*/, bool /*ok*/) {
        Draw(s, prim, start, count, indices);
    }
    virtual void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va,
                      uint32_t dst_points_va) = 0;
    virtual void Present(const Surface& front) = 0;
    // the CPU is about to touch this resource's memory: write GPU results back first
    virtual void Sync(uint32_t data_va, uint32_t size) = 0;
    // a lock that does not wait for the GPU (NOOVERWRITE): a result a frame or two old is what the console gives too
    virtual void SyncLate(uint32_t data_va) = 0;
    // SyncLate in two for the render thread: DeliverLate (any thread) writes the newest finished copy to guest memory,
    // false when there is none recent; RecordLate (in order with the draws) starts a copy for a later lock
    virtual bool DeliverLate(uint32_t) { return false; }
    virtual void RecordLate(uint32_t) {}
    virtual void Flush() = 0;
    // the render thread: a draw may put CPU work off (copying guest vertex bytes into upload memory) once
    // DeferWork(true) is called, until Settle, which the render thread calls before it tells the title the draws are
    // done (the title may then rewrite that guest memory); a backend runs it too before the GPU reads the memory
    virtual bool DeferWork(bool) { return false; }  // true: it does
    virtual void Settle() {}
    // as Flush, but the caller need not wait for it: the render thread runs it in turn (reports written once the GPU
    // is past them); a backend that draws at once just flushes
    virtual void Kick() { Flush(); }
    virtual void SetGamma(const uint8_t* ramp /*3 x 256*/) = 0;
    // visibility tests: draws count passing pixels while simple[kMCountEnable / 4] is set, ClearReport zeroes the
    // count, GetReport writes it to a 16 byte report once the GPU is past those draws; without them it stays pending
    virtual void ClearReport() {}
    virtual void GetReport(uint32_t /*report*/) {}

    // the XDK's fences and busy checks against the render thread; a backend that draws at once is never behind
    virtual uint32_t Fence() { return ++fence_; }
    virtual bool FencePending(uint32_t) { return false; }
    virtual void WaitFence(uint32_t) {}
    virtual bool Busy(uint32_t /*resource*/) { return false; }
    virtual void WaitResource(uint32_t /*resource*/) {}

private:
    uint32_t fence_ = 0;
};

std::unique_ptr<Backend> CreateBackend();
// Windows: the Direct3D 11 renderer, which CreateBackend returns when the settings ask for it
std::unique_ptr<Backend> CreateD3D11Backend();
// another renderer to try when the first one's Init fails (Direct3D 11 after 12), null when there is none
std::unique_ptr<Backend> CreateFallbackBackend();
// the same backend driven from a thread of its own: calls return once recorded
std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner);
// in-between frames for a title drawing one frame a step (GameInfo::step_interpolation)
std::unique_ptr<Backend> CreateInterpolating(std::unique_ptr<Backend> inner);

// perf diagnostic: the title's thread recording for the render thread, and waiting on it
inline std::atomic<uint64_t> g_perf_capture_ns{0}, g_perf_wait_ns{0}, g_perf_waits{0};
// the render thread: applying the batches' state changes, and running batches at all (with XBE_PERF)
inline std::atomic<uint64_t> g_perf_state_ns{0}, g_perf_batch_ns{0}, g_perf_prep_ns{0}, g_perf_record_ns{0};
// the render thread waiting for the record thread to drain (before a command that writes the list), and how often
inline std::atomic<uint64_t> g_perf_drain_ns{0}, g_perf_drains{0};
// the render thread's commands that made it wait for record, by kind: copy, present, read-back, late read-back, flush,
// other
inline std::atomic<uint64_t> g_perf_fence_ops[6] = {};
inline std::atomic<uint64_t> g_perf_wait_kind_ns[6];  // read-back, resource, fence, flush, copy, next frame
inline std::atomic<uint64_t> g_perf_sync_late{0}, g_perf_sync_block{0};  // read-backs the title waited for, by cause
inline std::atomic<uint32_t> g_shaders_compiled{0};  // D3DCompile calls (the shader cache missed), for the frames lines

// F3: save the next frame, every draw and target, under diag::CaptureDir
inline std::atomic<bool> g_capture{false};

}  // namespace gpu
