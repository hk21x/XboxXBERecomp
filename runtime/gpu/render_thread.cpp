// The Direct3D 12 backend on a thread of its own. The title's render thread records each call with what the console
// takes at call time (state changes, resource headers, indices, rects) and goes on; vertex, texture and surface memory
// are read when the draw runs, as the console's GPU reads them after the call returns. The XDK's fences, busy checks
// and locks wait for that.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <smmintrin.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "app/app.h"
#include "core/diag.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace gpu {

namespace {

// commands handed over at once, so the render thread keeps close behind and a wait is short
constexpr uint32_t kBatchCommands = 64;

// GameInfo::merge_draws: what one draw may grow to, so a long run still reaches the render thread in good time
constexpr uint32_t kMergedIndices = 1u << 13;

// resource headers the render thread reads: common, data, lock, format, size
constexpr uint32_t kHeaderBytes = 20, kHeaderSlot = 32;
constexpr uint32_t kHeaderRing = 1u << 20;  // guest memory for header copies; a copy is renewed after half a ring

enum class WaitFor : uint8_t { ReadBack, Resource, Fence, Flush, Copy, Frame, Count };

enum class Op : uint8_t { State, Draw, Clear, Copy, Present, Sync, RecordLate, Flush, Gamma, ClearReport, GetReport };

struct Cmd {
    Op op;
    uint32_t a = 0, b = 0, c = 0, d = 0;
    float f = 0;
    uint32_t data = 0, size = 0;  // bytes in Batch::bytes
    int32_t vs = -1, ps = -1;     // index in Batch::vs / ps: the shader changed
    bool joinable = false;        // a triangle list the next draw with the same state may add to (GameInfo::merge_draws)
    Surface s1, s2;
    uint64_t check = 0;           // XBE_CHECK_STATE: the title's state, hashed (CheckHash)
    int32_t pre = -1;             // a draw the prep thread assembled: its slot in Threaded::pre_ring_
    bool pre_ok = false;          // and Assemble's answer
};

struct Batch {
    uint64_t id = 0;
    std::vector<Cmd> cmds;
    std::vector<uint8_t> bytes;
    std::vector<std::shared_ptr<VertexShader>> vs;
    std::vector<std::shared_ptr<PixelShader>> ps;
};

// the plain data in State, around its shader pointers and the resource headers (sent as copies); of the transforms only
// view, projection, texture and world are drawn with
struct Range {
    size_t off, len;
};
size_t At(const State& s, const void* p) { return size_t(static_cast<const uint8_t*>(p) - reinterpret_cast<const uint8_t*>(&s)); }
std::vector<Range> StateRanges() {
    static const State s;
    return {
        {At(s, &s.simple), At(s, &s.textures) - At(s, &s.simple)},
        {At(s, &s.index_buffer), At(s, &s.vs) - At(s, &s.index_buffer)},
        {At(s, &s.vsc), At(s, &s.rt) - At(s, &s.vsc)},
        {At(s, &s.scissor_count), At(s, &s.transforms) - At(s, &s.scissor_count)},
        {At(s, &s.transforms[2]), 2 * sizeof(s.transforms[0])},
        {At(s, &s.transforms[16]), 4 * sizeof(s.transforms[0])},
        {At(s, &s.transforms[256]), 4 * sizeof(s.transforms[0])},  // WORLD to WORLD3: vertex blending
        {At(s, &s.material), sizeof(State) - At(s, &s.material)},
    };
}

// XBE_CHECK_STATE=1 (a test, headless too): every draw checks the render thread's copy of the state against the
// title's (the plain data of StateRanges, State::vsc_gen left out: the render thread's own), and that a vertex
// constant count (State::vsc_gen) the backend reuses uploads by always stands for the same constants
bool CheckState() {
    static const bool on = [] {
        const char* e = std::getenv("XBE_CHECK_STATE");
        bool v = e && *e == '1';
        if (v) LOGR("render thread: XBE_CHECK_STATE=1, every draw's state checked against the title's");
        return v;
    }();
    return on;
}

uint64_t Fnv(const uint8_t* p, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

uint64_t CheckHash(const State& s, const std::vector<Range>& ranges) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&s);
    const size_t gen = size_t(reinterpret_cast<const uint8_t*>(&s.vsc_gen) - b), gen_end = gen + sizeof(s.vsc_gen);
    uint64_t h = 1469598103934665603ull;
    for (const Range& r : ranges) {
        size_t from = r.off, to = r.off + r.len;
        if (gen >= from && gen_end <= to) {
            h = Fnv(b + from, gen - from, h);
            h = Fnv(b + gen_end, to - gen_end, h);
        } else {
            h = Fnv(b + from, to - from, h);
        }
    }
    return h;
}

// equal bytes, 16 at a time: most of the state is compared on every draw
bool Same(const uint8_t* a, const uint8_t* b, size_t n) {
    size_t i = 0;
    __m128i diff = _mm_setzero_si128();
    for (; i + 16 <= n; i += 16)
        diff = _mm_or_si128(diff, _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i)),
                                                _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i))));
    if (!_mm_testz_si128(diff, diff)) return false;
    return i == n || !std::memcmp(a + i, b + i, n - i);
}

using Clock = std::chrono::steady_clock;
uint64_t Ns(Clock::time_point t) { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count()); }

class Threaded final : public Backend {
public:
    explicit Threaded(std::unique_ptr<Backend> inner) : inner_(std::move(inner)), ranges_(StateRanges()) {
        ring_ = mem::AllocSystem(kHeaderRing, 64);
        cur_ = NewBatch();
        defer_ = inner_->DeferWork(true);
        pre_ = [] {
            const char* e = std::getenv("XBE_PRE_ASSEMBLE");
            bool on = !(e && *e == '0');
            LOGR("render thread: draws assembled %s", on ? "one step ahead on the prep thread" : "by the render thread (XBE_PRE_ASSEMBLE=0)");
            return on;
        }();
        if (pre_) {
            pre_ring_.resize(kPre);
            std::thread([this] { Prep(); }).detach();
        }
        record_ = [&] {
            const char* e = std::getenv("XBE_RECORD_THREAD");
            return !(e && *e == '0') && inner_->RecordOnThread([this] { Drain(); });
        }();
        if (record_) std::thread([this] { Record(); }).detach();
        std::thread([this] { Run(); }).detach();
    }
    ~Threaded() override {
        {
            std::lock_guard lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        std::unique_lock lock(m_);
        // at exit the worker may be gone mid-batch: then the inner backend is left, not freed under it
        if (!done_cv_.wait_for(lock, std::chrono::milliseconds(500), [&] { return exited_; })) (void)inner_.release();
    }

    bool Init(uint32_t width, uint32_t height) override { return inner_->Init(width, height); }

    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        auto t = Clock::now();
        SendState(s);
        if (Mergeable(s, prim, count, indices)) {
            PutMerged(prim, start, count, indices);
        } else {
            Cmd c{Op::Draw};
            c.a = prim;
            c.b = start;
            c.c = count;
            if (indices) {
                c.d = 1;
                c.data = Put(indices, size_t(count) * 2);
            }
            cur_->cmds.push_back(c);
        }
        if (CheckState()) cur_->cmds.back().check = CheckHash(s, ranges_);
        for (uint32_t tex : s.textures) Use(tex);
        for (uint32_t pal : s.palettes) Use(pal);
        for (const auto& st : s.streams) Use(st.vb);
        if (s.input_active)
            for (const auto& st : s.input_streams) Use(st.vb);
        Use(s.rt.header);
        Use(s.zs.header);
        Written(s.rt);
        if (cur_->cmds.size() >= kBatchCommands) Submit();
        g_perf_capture_ns += Ns(t);
    }

    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) override {
        auto t = Clock::now();
        SendState(s);
        Cmd c{Op::Clear};
        c.a = count;
        c.b = flags;
        c.c = color;
        c.d = stencil;
        c.f = z;
        if (count && rects) {
            c.data = Put(rects, size_t(count) * 16);
            c.size = count * 16;
        }
        cur_->cmds.push_back(c);
        Use(s.rt.header);
        Use(s.zs.header);
        if (flags & kClearTarget) Written(s.rt);
        g_perf_capture_ns += Ns(t);
    }

    // rects and points are taken now; the copy itself, CPU or GPU, runs in order like the console's
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override {
        auto t = Clock::now();
        Cmd c{Op::Copy};
        c.a = count;
        c.b = count && src_rects ? Scratch(kernel::HostPtr(src_rects), count * 16) : 0;
        c.c = count && dst_points ? Scratch(kernel::HostPtr(dst_points), count * 8) : 0;
        c.s1 = src;
        c.s1.header = Header(src.header);
        c.s2 = dst;
        c.s2.header = Header(dst.header);
        cur_->cmds.push_back(c);
        Use(src.header);
        Use(dst.header);
        Written(dst);
        g_perf_capture_ns += Ns(t);
    }

    void Present(const Surface& front) override {
        auto t = Clock::now();
        Cmd c{Op::Present};
        c.s1 = front;
        c.s1.header = Header(front.header);
        cur_->cmds.push_back(c);
        Use(front.header);
        uint64_t id = Submit();
        g_perf_capture_ns += Ns(t);
        // the title records one frame while the one before it draws
        Wait(last_present_, WaitFor::Frame, nullptr);
        last_present_ = id;
    }

    void Sync(uint32_t data, uint32_t) override { ReadBack(data, false); }

    void SyncLate(uint32_t data) override {
        if (!inner_->DeliverLate(data)) {
            if (diag::Perf()) LOG_FIRST(16, "render thread: no finished copy of %08X for a NOOVERWRITE lock, reading it back now", data);
            written_.insert(data);
            ReadBack(data, true);
        }
        Cmd c{Op::RecordLate};
        c.a = data;
        cur_->cmds.push_back(c);
    }

    void Flush() override {
        cur_->cmds.push_back(Cmd{Op::Flush});
        Wait(Submit(), WaitFor::Flush, "flush");
    }
    void Kick() override {
        cur_->cmds.push_back(Cmd{Op::Flush});
        Submit();
    }

    void SetGamma(const uint8_t* ramp) override {
        Cmd c{Op::Gamma};
        c.data = Put(ramp, 768);
        cur_->cmds.push_back(c);
    }

    void ClearReport() override { cur_->cmds.push_back(Cmd{Op::ClearReport}); }
    void GetReport(uint32_t report) override {
        Cmd c{Op::GetReport};
        c.a = report;
        cur_->cmds.push_back(c);
    }

    uint32_t Fence() override { return uint32_t(cur_->id); }
    bool FencePending(uint32_t fence) override { return fence > done_.load(); }
    void WaitFence(uint32_t fence) override { Wait(fence, WaitFor::Fence, "fence"); }
    bool Busy(uint32_t resource) override {
        auto it = last_use_.find(resource);
        return it != last_use_.end() && it->second > done_.load();
    }
    void WaitResource(uint32_t resource) override {
        auto it = last_use_.find(resource);
        if (it != last_use_.end()) Wait(it->second, WaitFor::Resource, "resource");
    }

private:
    Batch* NewBatch() {
        Batch* b;
        {
            std::lock_guard lock(m_);
            if (free_.empty()) {
                b = new Batch;
            } else {
                b = free_.back();
                free_.pop_back();
            }
        }
        b->id = ++next_id_;
        return b;
    }

    // GameInfo::merge_draws: indexed triangles of the 3D world. Not 2D, whose squeezing looks at each draw's own vertices;
    // not programs, which may be 2D; not a visibility test, counted per draw; not flat shading, whose colour comes from
    // a vertex the list might not keep first
    bool Mergeable(const State& s, uint32_t prim, uint32_t count, const uint8_t* indices) const {
        constexpr uint32_t kFlat = 0x1D00;
        // vertex programs too with XBE_MERGE_DRAWS=2 (experiment): their constants are state, so a run has the same ones
        // (not 2D: a title whose programs draw it squeezed is left out, merge_programs_)
        return merge_ && indices && (prim == 5 || prim == 6) && count >= 3 && (!(s.vs_handle & 1) || merge_programs_) &&
               (s.vs_handle & 0xE) != 0x4 &&
               !s.input_active && !s.blank_format && !s.simple[kMCountEnable >> 2] && s.simple[kMShadeMode >> 2] != kFlat;
    }

    // the draw as a triangle list, added to the command before it when nothing changed since: no state command came
    // between them, so they differ only in which vertices they draw, and a list keeps their order and each one's winding
    void PutMerged(uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
        const uint32_t tris = prim == 5 ? count / 3 : count - 2;
        const uint32_t n = tris * 3;
        std::vector<uint8_t>& bytes = cur_->bytes;
        Cmd* last = cur_->cmds.empty() ? nullptr : &cur_->cmds.back();
        const bool join = last && last->joinable && last->b == start && last->c + n <= kMergedIndices &&
                          bytes.size() == size_t(last->data) + size_t(last->c) * 2;
        size_t at = join ? bytes.size() : (bytes.size() + 3) & ~size_t(3);
        bytes.resize(at + size_t(n) * 2);
        uint8_t* out = bytes.data() + at;
        if (prim == 5) {
            std::memcpy(out, indices, size_t(n) * 2);
        } else {
            for (uint32_t i = 0; i < tris; i++, out += 6) {
                std::memcpy(out, indices + size_t(i + (i & 1)) * 2, 2);
                std::memcpy(out + 2, indices + size_t(i + 1 - (i & 1)) * 2, 2);
                std::memcpy(out + 4, indices + size_t(i + 2) * 2, 2);
            }
        }
        if (join) {
            last->c += n;
            return;
        }
        Cmd c{Op::Draw};
        c.a = 5;
        c.b = start;
        c.c = n;
        c.d = 1;
        c.data = uint32_t(at);
        c.joinable = true;
        cur_->cmds.push_back(c);
    }

    uint32_t Put(const void* p, size_t n) {
        size_t at = (cur_->bytes.size() + 3) & ~size_t(3);
        cur_->bytes.resize(at + n);
        std::memcpy(cur_->bytes.data() + at, p, n);
        return uint32_t(at);
    }

    void ReadBack(uint32_t data, bool late) {
        // only surfaces the GPU wrote since the last read-back hold anything newer than guest memory
        if (!written_.erase(data)) return;
        (late ? g_perf_sync_late : g_perf_sync_block)++;
        if (diag::Perf() && logged_.insert(uint64_t(data) << 1 | (late ? 1 : 0)).second)
            LOGI("render thread: read-back of %08X for %s", data, late ? "a NOOVERWRITE lock without a finished copy" : "a lock / BlockOnResource");
        Cmd c{Op::Sync};
        c.a = data;
        cur_->cmds.push_back(c);
        Wait(Submit(), WaitFor::ReadBack, "read-back");
    }

    // bytes the render thread reads through a guest address, in the header ring
    uint32_t Scratch(const void* p, uint32_t n) {
        uint32_t size = (n + kHeaderSlot - 1) & ~(kHeaderSlot - 1);
        if (ring_pos_ % kHeaderRing + size > kHeaderRing) ring_pos_ += kHeaderRing - ring_pos_ % kHeaderRing;
        uint32_t at = ring_ + uint32_t(ring_pos_ % kHeaderRing);
        ring_pos_ += size;
        std::memcpy(kernel::HostPtr(at), p, n);
        return at;
    }

    // A header as it is now. The console's push buffer takes a texture's format and address, and a surface's or vertex
    // buffer's address, when they are set; the title points the same header elsewhere before the GPU gets there.
    uint32_t Header(uint32_t header) {
        if (!header || !ring_) return header;
        // vertex buffer, index buffer and palette headers are 12 bytes: reading 20 can run off the end of the heap
        const uint32_t type = kernel::R32(header) & 0x70000;  // 0 vertex, 0x10000 index, 0x30000 palette
        uint8_t now[kHeaderBytes] = {};
        std::memcpy(now, kernel::HostPtr(header), type < 0x20000 || type == 0x30000 ? 12 : kHeaderBytes);
        Recent<HeaderCopy>& r = recent_headers_[(header >> 3) & 63];
        HeaderCopy& c = r.key == header ? *r.value : headers_[header];
        r = {header, &c};
        if (c.made && !std::memcmp(c.bytes, now, kHeaderBytes) && ring_pos_ - c.made < kHeaderRing / 2) return c.at;
        c.at = Scratch(now, kHeaderBytes);
        c.made = ring_pos_;
        std::memcpy(c.bytes, now, kHeaderBytes);
        return c.at;
    }

    // a field of the render thread's State, when it differs from what was sent
    void Delta(const void* field, const void* value, size_t n) {
        uint8_t* sent = reinterpret_cast<uint8_t*>(&sent_) + At(sent_, field);
        if (Same(sent, static_cast<const uint8_t*>(value), n)) return;
        Send(At(sent_, field), value, n);
    }
    // n bytes at offset off of the State, changed: kept as sent and put in the batch
    void Send(size_t off, const void* value, size_t n) {
        std::memcpy(reinterpret_cast<uint8_t*>(&sent_) + off, value, n);
        uint32_t head[2] = {uint32_t(off), uint32_t(n)};
        size_t at = cur_->bytes.size();
        cur_->bytes.resize(at + 8 + n);
        std::memcpy(cur_->bytes.data() + at, head, 8);
        std::memcpy(cur_->bytes.data() + at + 8, value, n);
    }

    // what changed since the last command, in 64 byte pieces
    void SendState(const State& s) {
        const uint8_t* now = reinterpret_cast<const uint8_t*>(&s);
        Cmd c{Op::State};
        size_t first = cur_->bytes.size();
        uint8_t* sent = reinterpret_cast<uint8_t*>(&sent_);
        for (const Range& r : ranges_) {
            // whole 64 byte pieces in one pass, stopping only at one that changed; the same pieces Delta would send
            size_t off = r.off, end = r.off + r.len;
            for (; off + 64 <= end; off += 64) {
                const __m128i* a = reinterpret_cast<const __m128i*>(sent + off);
                const __m128i* b = reinterpret_cast<const __m128i*>(now + off);
                __m128i d = _mm_or_si128(_mm_or_si128(_mm_xor_si128(_mm_loadu_si128(a), _mm_loadu_si128(b)),
                                                      _mm_xor_si128(_mm_loadu_si128(a + 1), _mm_loadu_si128(b + 1))),
                                         _mm_or_si128(_mm_xor_si128(_mm_loadu_si128(a + 2), _mm_loadu_si128(b + 2)),
                                                      _mm_xor_si128(_mm_loadu_si128(a + 3), _mm_loadu_si128(b + 3))));
                if (!_mm_testz_si128(d, d)) Send(off, now + off, 64);
            }
            if (off < end) Delta(sent + off, now + off, end - off);
        }
        std::array<uint32_t, 4> textures;
        for (size_t i = 0; i < 4; i++) textures[i] = Header(s.textures[i]);
        Delta(&sent_.textures, &textures, sizeof(textures));
        std::array<uint32_t, 4> palettes;
        for (size_t i = 0; i < 4; i++) palettes[i] = Header(s.palettes[i]);
        Delta(&sent_.palettes, &palettes, sizeof(palettes));
        auto streams = s.streams;
        for (auto& st : streams) st.vb = Header(st.vb);
        Delta(&sent_.streams, &streams, sizeof(streams));
        auto input_streams = s.input_streams;
        for (auto& st : input_streams) st.vb = s.input_active ? Header(st.vb) : 0;
        Delta(&sent_.input_streams, &input_streams, sizeof(input_streams));
        Surface rt = s.rt, zs = s.zs;
        rt.header = Header(rt.header);
        zs.header = Header(zs.header);
        Delta(&sent_.rt, &rt, sizeof(rt));
        Delta(&sent_.zs, &zs, sizeof(zs));
        if (s.vs.get() != sent_.vs.get()) {
            sent_.vs = s.vs;
            c.vs = int32_t(cur_->vs.size());
            cur_->vs.push_back(s.vs);
        }
        if (s.ps.get() != sent_.ps.get()) {
            sent_.ps = s.ps;
            c.ps = int32_t(cur_->ps.size());
            cur_->ps.push_back(s.ps);
        }
        if (cur_->bytes.size() == first && c.vs < 0 && c.ps < 0) return;
        c.data = uint32_t(first);
        c.size = uint32_t(cur_->bytes.size() - first);
        cur_->cmds.push_back(c);
    }

    // resources by guest header: the batch that last read them
    void Use(uint32_t header) {
        if (!header) return;
        Recent<uint64_t>& r = recent_uses_[(header >> 3) & 63];
        if (r.key != header) r = {header, &last_use_[header]};
        *r.value = cur_->id;
    }
    void Written(const Surface& s) {
        if (s.header) written_.insert(mem::VirtualFromPhysical(s.data));
    }

    uint64_t Submit() {
        uint64_t id = cur_->id;
        {
            std::lock_guard lock(m_);
            queue_.push_back(cur_);
        }
        cv_.notify_one();
        cur_ = NewBatch();
        return id;
    }

    void Wait(uint64_t id, WaitFor kind, const char* why) {
        if (!id || done_.load() >= id) return;
        if (id >= cur_->id) id = Submit();
        auto t = Clock::now();
        wanted_.store(id);  // the render thread settles once it has run this batch, not after more
        {
            std::unique_lock lock(m_);
            done_cv_.wait(lock, [&] { return done_.load() >= id; });
        }
        wanted_.store(~0ull);
        uint64_t ns = Ns(t);
        g_perf_wait_ns += ns;
        g_perf_wait_kind_ns[size_t(kind)] += ns;
        if (why) {
            g_perf_waits++;
            if (diag::Perf()) LOG_FIRST(32, "render thread: the title waited %.2f ms for a %s", ns / 1e6, why);
        }
    }

    // a batch the render thread (or, with the pipeline, the record thread) has finished: done once the backend
    // settled the work its draws put off. That is done for several batches together: when no batch waits, after kHold
    // of them, or once the title waits for one already run
    void SettleBatches() {
        inner_->Settle();
        {
            std::lock_guard lock(m_);
            done_.store(unsettled_);
        }
        done_cv_.notify_all();
        unsettled_ = 0;
        held_ = 0;
    }
    void Complete(Batch* b) {
        uint64_t id = b->id;
        b->cmds.clear();
        b->bytes.clear();
        b->vs.clear();
        b->ps.clear();
        {
            std::lock_guard lock(m_);
            free_.push_back(b);
            if (!defer_) done_.store(id);
        }
        if (!defer_) {
            done_cv_.notify_all();
            return;
        }
        unsettled_ = id;
        if (++held_ >= kHold || wanted_.load() <= id) SettleBatches();
    }

    void Run() {
        NameThread(L"render");
        // batches from the prep thread when it runs, else straight from the title
        std::deque<Batch*>& source = pre_ ? ready_ : queue_;
        std::condition_variable& source_cv = pre_ ? ready_cv_ : cv_;
        for (;;) {
            Batch* b;
            {
                std::unique_lock lock(m_);
                if (!record_ && source.empty() && unsettled_) {
                    lock.unlock();
                    SettleBatches();
                    continue;
                }
                source_cv.wait(lock, [&] { return (pre_ ? prep_exited_ : stop_) || !source.empty(); });
                if (source.empty()) break;
                b = source.front();
                source.pop_front();
            }
            static const bool perf = diag::Perf();
            auto t = perf ? Clock::now() : Clock::time_point{};
            run_batch_ = b;
            seg_from_ = 0;
            for (size_t i = 0; i < b->cmds.size(); i++) {
                const Cmd& c = b->cmds[i];
                run_cmd_ = i;
                // with the pipeline, anything but a state change or a draw writes the list: record drains first
                if (record_ && c.op != Op::State && c.op != Op::Draw) {
                    PushSegment(i, false);
                    WaitRecorded();
                }
                Execute(*b, c);
            }
            if (perf) g_perf_batch_ns += Ns(t);
            uint64_t id = b->id;
            if (pre_) {
                {
                    std::lock_guard lock(m_);
                    ran_ = id;
                }
                ran_cv_.notify_all();
            }
            if (record_) {
                PushSegment(b->cmds.size(), true);  // record finishes the batch
                run_batch_ = nullptr;
            } else {
                run_batch_ = nullptr;
                Complete(b);
            }
        }
        if (record_) {
            {
                std::lock_guard lock(m_);
                run_exited_ = true;
            }
            seg_cv_.notify_all();
            return;
        }
        std::lock_guard lock(m_);
        exited_ = true;
        done_cv_.notify_all();
    }

    // The pipeline's record thread: the render thread hands it each batch in segments (up to a command that writes
    // the list, and the rest with the batch's last); it keeps its own copy of the state from the segments' changes and
    // records their draws, then finishes the batch (the title may then reuse what its draws read)
    struct Segment {
        Batch* b;
        size_t from, to;
        bool last;
    };
    void PushSegment(size_t to, bool last) {
        if (to <= seg_from_ && !last) return;
        {
            std::lock_guard lock(m_);
            segments_.push_back({run_batch_, seg_from_, to, last});
            segs_pushed_++;
        }
        seg_from_ = to;
        seg_cv_.notify_one();
    }
    // the render thread: once record has done every segment handed to it
    void WaitRecorded() {
        std::unique_lock lock(m_);
        seg_done_cv_.wait(lock, [&] { return segs_done_ == segs_pushed_; });
    }
    // the backend's drain: what the draw being decided follows goes to record, and the render thread waits for it
    void Drain() {
        if (run_batch_) PushSegment(run_cmd_, false);
        WaitRecorded();
    }
    void Record() {
        NameThread(L"render record");
        for (;;) {
            Segment g;
            {
                std::unique_lock lock(m_);
                if (segments_.empty() && unsettled_) {
                    lock.unlock();
                    SettleBatches();
                    continue;
                }
                seg_cv_.wait(lock, [&] { return run_exited_ || !segments_.empty(); });
                if (segments_.empty()) break;
                g = segments_.front();
                segments_.pop_front();
            }
            static const bool perf = diag::Perf();
            auto t = perf ? Clock::now() : Clock::time_point{};
            for (size_t i = g.from; i < g.to; i++) {
                const Cmd& c = g.b->cmds[i];
                if (c.op == Op::State) {
                    if (ApplyState(rec_state_, *g.b, c) || !rec_vsc_count_) rec_vsc_count_++;
                    rec_state_.vsc_gen = rec_vsc_count_;
                } else if (c.op == Op::Draw) {
                    inner_->RecordNext(rec_state_);
                    if (c.pre >= 0) pre_used_.fetch_add(1, std::memory_order_release);  // its slot is free now
                }
            }
            if (g.last) Complete(g.b);
            if (perf) g_perf_record_ns += Ns(t);
            {
                std::lock_guard lock(m_);
                segs_done_++;
            }
            seg_done_cv_.notify_all();
        }
        std::lock_guard lock(m_);
        exited_ = true;
        done_cv_.notify_all();
    }

    void Check(const Cmd& c) {
        static uint64_t draws = 0, wrong = 0, stale = 0, last_gen = 0, last_vsc = 0;
        draws++;
        if (CheckHash(state_, ranges_) != c.check && wrong++ < 8)
            LOGR("render thread: check: draw %llu's state differs from the title's", (unsigned long long)draws);
        uint64_t vsc = Fnv(reinterpret_cast<const uint8_t*>(state_.vsc.data()), sizeof(state_.vsc), 1469598103934665603ull);
        // the count only goes up: one that goes back may name constants an older upload held
        if (((state_.vsc_gen == last_gen && vsc != last_vsc) || state_.vsc_gen < last_gen) && stale++ < 8)
            LOGR("render thread: check: draw %llu has count %llu with other vertex constants", (unsigned long long)draws,
                 (unsigned long long)state_.vsc_gen);
        last_gen = state_.vsc_gen;
        last_vsc = vsc;
        if (draws % 200000 == 0)
            LOGR("render thread: check: %llu draws, %llu with a different state, %llu with a stale vertex constant count",
                 (unsigned long long)draws, (unsigned long long)wrong, (unsigned long long)stale);
    }

    // a state command's changes into st; true when they touched the vertex constants
    static bool ApplyState(State& st, const Batch& b, const Cmd& c) {
        const uint8_t* data = b.bytes.data() + c.data;
        uint8_t* s = reinterpret_cast<uint8_t*>(&st);
        static const State layout;
        static const size_t vsc_from = At(layout, &layout.vsc), vsc_to = vsc_from + sizeof(layout.vsc);
        bool vsc = false;
        for (const uint8_t* p = data; p < data + c.size;) {
            uint32_t head[2];
            std::memcpy(head, p, 8);
            std::memcpy(s + head[0], p + 8, head[1]);
            vsc |= head[0] < vsc_to && head[0] + head[1] > vsc_from;
            p += 8 + head[1];
        }
        if (c.vs >= 0) st.vs = b.vs[size_t(c.vs)];
        if (c.ps >= 0) st.ps = b.ps[size_t(c.ps)];
        return vsc;
    }

    // The prep thread, one step before the render thread (XBE_PRE_ASSEMBLE, on unless 0): it takes the title's batches
    // first, keeps its own copy of the state and assembles their draws (gpu::Assemble reads only the state, the
    // indices and guest memory) into pre_ring_, then hands the batch on. A command that may write guest memory on the
    // render thread (all but state, draws, clears, gamma and report clears: read-backs, copies, presents, reports)
    // leaves its batch's later draws to the render thread, and the next batch waits until the render thread has run
    // this one: a draw never reads vertices from before such a command.
    void Prep() {
        NameThread(L"render prep");
        for (;;) {
            Batch* b;
            {
                std::unique_lock lock(m_);
                cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                if (queue_.empty()) break;
                b = queue_.front();
                queue_.pop_front();
            }
            if (wait_for_) {
                std::unique_lock lock(m_);
                ran_cv_.wait(lock, [&] { return ran_ >= wait_for_; });
                wait_for_ = 0;
            }
            static const bool perf = diag::Perf();
            auto t = perf ? Clock::now() : Clock::time_point{};
            bool ahead = true;
            for (Cmd& c : b->cmds) {
                if (c.op == Op::State) {
                    ApplyState(prep_state_, *b, c);
                } else if (c.op == Op::Draw) {
                    if (!ahead) continue;
                    // a slot is free once the render thread drew what it held (kPre draws ago)
                    while (pre_made_ - pre_used_.load(std::memory_order_acquire) >= kPre) std::this_thread::yield();
                    size_t slot = size_t(pre_made_ % kPre);
                    c.pre_ok = Assemble(prep_state_, c.a, c.b, c.c, c.d ? b->bytes.data() + c.data : nullptr, pre_ring_[slot]);
                    c.pre = int32_t(slot);
                    pre_made_++;
                } else if (c.op != Op::Clear && c.op != Op::Gamma && c.op != Op::ClearReport) {
                    ahead = false;
                    wait_for_ = b->id;
                }
            }
            if (perf) g_perf_prep_ns += Ns(t);
            {
                std::lock_guard lock(m_);
                ready_.push_back(b);
            }
            ready_cv_.notify_one();
        }
        {
            std::lock_guard lock(m_);
            prep_exited_ = true;
        }
        ready_cv_.notify_all();
    }

    static void NameThread(const wchar_t* name) {
        using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        if (auto set = reinterpret_cast<SetDescription>(
                reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))))
            set(GetCurrentThread(), name);
    }

    void Execute(const Batch& b, const Cmd& c) {
        const uint8_t* data = b.bytes.data() + c.data;
        switch (c.op) {
            case Op::State: {
                static const bool perf = diag::Perf();
                auto t = perf ? Clock::now() : Clock::time_point{};
                // the count is the render thread's own, set again after every change: a change sent in whole 64 byte
                // chunks carries the title's 0 over it (it sits next to vsc)
                if (ApplyState(state_, b, c) || !vsc_count_) vsc_count_++;
                state_.vsc_gen = vsc_count_;
                if (perf) g_perf_state_ns += Ns(t);
                break;
            }
            case Op::Draw:
                if (CheckState()) Check(c);
                if (c.pre >= 0) {
                    inner_->DrawAssembled(state_, c.a, c.b, c.c, c.d ? data : nullptr, pre_ring_[size_t(c.pre)], c.pre_ok);
                    if (!record_) pre_used_.fetch_add(1, std::memory_order_release);  // else once recorded
                } else {
                    inner_->Draw(state_, c.a, c.b, c.c, c.d ? data : nullptr);
                }
                break;
            case Op::Clear: inner_->Clear(state_, c.a, c.size ? data : nullptr, c.b, c.c, c.f, c.d); break;
            case Op::Copy: inner_->Copy(c.s1, c.s2, c.a, c.b, c.c); break;
            case Op::Present: inner_->Present(c.s1); break;
            case Op::Sync: inner_->Sync(c.a, 0); break;
            case Op::RecordLate: inner_->RecordLate(c.a); break;
            case Op::Flush: inner_->Flush(); break;
            case Op::Gamma: inner_->SetGamma(data); break;
            case Op::ClearReport: inner_->ClearReport(); break;
            case Op::GetReport: inner_->GetReport(c.a); break;
        }
    }

    std::unique_ptr<Backend> inner_;
    std::vector<Range> ranges_;
    const bool merge_ = [] {
        const char* e = std::getenv("XBE_MERGE_DRAWS");
        bool on = e && *e ? *e != '0' : app::kGame.merge_draws;
        if (on) LOGR("gpu: runs of draws with the same state are merged");
        return on;
    }();
    // not for a title whose programs draw 2D the renderers squeeze per draw (GameInfo::squeeze_screen_programs)
    const bool merge_programs_ = [] {
        const char* e = std::getenv("XBE_MERGE_DRAWS");
        bool on = e && *e == '2' && !app::kGame.squeeze_screen_programs;
        if (on) LOGR("gpu: XBE_MERGE_DRAWS=2, vertex program draws are merged too (experiment)");
        return on;
    }();

    // the title's thread
    struct HeaderCopy {
        uint32_t at = 0;
        uint64_t made = 0;  // ring_pos_ after it was written
        uint8_t bytes[kHeaderBytes];
    };
    std::unordered_map<uint32_t, HeaderCopy> headers_;
    // the last few lookups, in front of the maps (their elements stay put)
    template <typename T>
    struct Recent {
        uint32_t key = 0;
        T* value = nullptr;
    };
    std::array<Recent<HeaderCopy>, 64> recent_headers_{};
    std::array<Recent<uint64_t>, 64> recent_uses_{};
    uint32_t ring_ = 0;
    uint64_t ring_pos_ = 0;
    Batch* cur_ = nullptr;
    State sent_;  // the state as the render thread will have it
    std::unordered_map<uint32_t, uint64_t> last_use_;
    std::unordered_set<uint32_t> written_;  // surfaces drawn since their last read-back, by data address
    std::unordered_set<uint64_t> logged_;   // perf diagnostic: read-backs named once, by address and kind
    uint64_t next_id_ = 0, last_present_ = 0;

    // the render thread
    State state_;

    std::mutex m_;
    std::condition_variable cv_, done_cv_;
    std::deque<Batch*> queue_;
    std::vector<Batch*> free_;
    std::atomic<uint64_t> done_{0};
    uint64_t vsc_count_ = 0;  // vertex constant changes so far (State::vsc_gen), only ever up
    std::atomic<uint64_t> wanted_{~0ull};  // the batch the title waits for
    bool defer_ = false;                   // the backend puts work off until Settle
    static constexpr uint32_t kHold = 16;  // batches run before their put off work is settled
    bool stop_ = false, exited_ = false;
    // the prep thread (Prep): batches it has assembled, waiting for the render thread
    bool pre_ = false, prep_exited_ = false;
    static constexpr uint64_t kPre = 2048;  // assembled draws in flight, many batches' worth
    std::vector<Assembled> pre_ring_;
    uint64_t pre_made_ = 0;                // the prep thread's
    std::atomic<uint64_t> pre_used_{0};    // the render thread's
    State prep_state_;
    std::deque<Batch*> ready_;
    std::condition_variable ready_cv_;
    uint64_t ran_ = 0;       // the last batch the render thread ran (under m_)
    // the pipeline's record thread (Record), when the backend records on it (RecordOnThread)
    bool record_ = false, run_exited_ = false;
    std::deque<Segment> segments_;
    uint64_t segs_pushed_ = 0, segs_done_ = 0;  // under m_
    std::condition_variable seg_cv_, seg_done_cv_;
    Batch* run_batch_ = nullptr;  // the render thread's: the batch it runs, its command, the next segment's start
    size_t run_cmd_ = 0, seg_from_ = 0;
    State rec_state_;
    uint64_t rec_vsc_count_ = 0;
    uint64_t unsettled_ = 0;  // the batches finished but not settled yet (whichever thread finishes batches)
    uint32_t held_ = 0;
    uint64_t wait_for_ = 0;  // the prep thread's: a batch that may write guest memory, run before the next is assembled
    std::condition_variable ran_cv_;
};

}  // namespace

std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner) { return std::make_unique<Threaded>(std::move(inner)); }

}  // namespace gpu
