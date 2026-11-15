// In-between frames for a title that draws one frame a step of its game (GameInfo::step_interpolation). Each step's clears, draws
// and copies are kept as they pass; when it presents, the step is drawn again a few times with the vertex shader constants blended
// from the step before, by the time since it presented, at the rate of the frame cap, until the title has to go on. A
// draw of the step before is found by what it draws, in order. Its memory is the title's until its next step, and the title is
// waiting here. So the picture is up to one step late; nothing is drawn beyond what the title gave. No blend after a camera cut.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "app/app.h"
#include "core/diag.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/texture.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace gpu {
namespace {
using Clock = std::chrono::steady_clock;
constexpr double kLongestStep = 0.15, kShortestStep = 0.005;  // seconds; past these the steps are not moving a picture along
constexpr double kMargin = 0.002;                             // seconds kept for the title's own work before its first draw
constexpr int kMostFrames = 32;
constexpr double kSameMove = 0.1, kStill = 1e-4;              // of the camera's move, and for no move at all: the same camera
constexpr int kLeastVotes = 3;                                // draws agreeing on it
constexpr size_t kSampleDraws = 192, kMostMoves = 24;
constexpr uint32_t kLeastVertices = 24;
constexpr int kFreshShare = 4;                                // matched draws of a program to each new one, for it to be scenery
constexpr double kLate = 1.2;                                 // of the usual step
constexpr int kFirstPatience = 120, kMostPatience = 1800;     // steps
constexpr int kLateWeight = 10, kLateLimit = 60;              // a late step counts this, a step takes one off: about 1 late in 10, held
constexpr double kOverrun = 0.001;                            // seconds past their time a step's frames may run
constexpr double kTooLong = 0.6;                              // of a step, a replay's time
constexpr double kOffCamera = 0.0025;  // a still scenery draw's in-between place this far off the camera's (screen widths)
constexpr float kFar = 0.25f, kLeap = 4.0f;                   // how far a matrix moves in a step and is still the same draw
constexpr float kStray = 0.25f, kStrayDepth = 0.15f;          // of the screen and of its depth: matched where the camera's move does not put it
constexpr uint32_t kFastest = 480;                            // frames a second for an unlimited cap
constexpr uint32_t kHeaderBytes = 20;                         // a resource header: common, data, lock, format, size
constexpr uint32_t kCopyBytes = 64u << 10;                    // a draw's vertices kept when no more than this
constexpr uint32_t kRegionBytes = 8u << 20, kRegionHeaders = 1u << 16;  // kept vertices of a step, in guest memory, two steps' worth
constexpr uint32_t kRegisters = 192, kMatrixRegisters = 4;    // the title's transform (GameInfo::interp_transform), four rows
// a record's constants are kept turned round so that the transform is first, wherever the title has it: Rec::vsc[k] is the
// title's register Transform() + k
uint32_t Transform() {
    static const uint32_t t = uint32_t(std::clamp(app::kGame.interp_transform + 96, 0, int(kRegisters - kMatrixRegisters)));
    return t;
}
constexpr float kConstantLeap = 0.05f;                        // a constant past the transform changed this much, and by half, is not blended

struct Rec {
    enum Kind : uint8_t { Clear, Draw, Copy } kind = Draw;
    std::unique_ptr<State> s;  // Clear, Draw; its registers and streams are changed for a replay, and put back
    using Constants = std::array<std::array<float, 4>, 192>;
    Constants vsc;  // turned round: [k] is the title's register Transform() + k
    uint32_t a = 0, b = 0, c = 0, d = 0;
    float z = 0;
    std::vector<uint8_t> bytes;  // indices, rects
    bool has_bytes = false;
    // the headers of what it reads: a title points one vertex buffer at each draw's vertices in turn
    struct Header {
        uint32_t va;
        uint8_t bytes[kHeaderBytes];
    };
    std::vector<Header> headers;
    // its vertices: a title rewrites one small buffer for each draw, waiting for the last, so memory has only the last by now
    struct Kept {
        uint8_t stream;
        uint32_t offset, bytes;
    };
    std::vector<Kept> kept;
    Surface s1, s2;
    // the targets' format and size words as the step drew into them, for the replays (Surface::as_drawn)
    uint32_t rt_format = 0, rt_size = 0, zs_format = 0, zs_size = 0;
    int prev = -1;  // the same draw in the step before
    bool turning = false;  // only the matrix's place follows prev
    bool derived = false;  // first seen this step: where the camera's move says its matrix was
    bool stray = false;    // its nearest copy of the step before is not where the camera's move puts it: still scenery come into view
    float was[4][4];
    int peer = -1;  // none found: a draw of this step sharing its matrix, whose blend it follows register by register
    // the constants that differ from prev's and are blended (register * 4 + component), worked out once a step for its replays:
    // deciding it for all 768 of every draw in every replay was three quarters of a replay's time
    std::vector<uint16_t> lerp;
    uint32_t lerp_step = 0;
};

uint64_t KeyOf(const Rec& r) {
    const State& s = *r.s;
    uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001B3ull; };
    mix(r.a), mix(r.b), mix(r.c), mix(s.vs_handle), mix(s.rt.header);
    // and the target's memory: a title draws one surface header into two pictures on alternate steps (EoN's driving: its
    // environment maps at 811C0000 and 813C0000 through one header), and a draw matched to the other picture's of the step
    // before blended two views into one. Unmatched, they are drawn at the step's place, and what samples them with them
    // (GameInfo::interp_sampled_targets_at_step)
    mix(s.rt.data);
    // stage 0's texture only: the other stages can hold a binding the program does not read that changes from one step to the
    // next (EoN's driving: stage 2 bound one step and not the next), and lost the match of a whole floor's scenery and its decals
    mix(s.textures[0]);
    // not the buffers: a title hands a mesh another vertex buffer from one step to the next (what it draws is the same); the matrix tells alike apart
    mix(s.viewport.x), mix(s.viewport.y), mix(s.viewport.w), mix(s.viewport.h), mix(s.squeeze_beyond);
    return h;
}

class Interpolating final : public Backend {
public:
    explicit Interpolating(std::unique_ptr<Backend> inner) : inner_(std::move(inner)) {}
    bool Init(uint32_t w, uint32_t h) override { return inner_->Init(w, h); }
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) override {
        inner_->Clear(s, count, rects, flags, color, z, stencil);
        if (!On()) return;
        Rec& r = Next(Rec::Clear);
        Keep(r, s);
        r.a = count, r.b = flags, r.c = color, r.d = stencil, r.z = z;
        AsDrawn(r, s);
        r.has_bytes = rects != nullptr;
        if (rects) r.bytes.assign(rects, rects + size_t(count) * 16);
    }
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        inner_->Draw(s, prim, start, count, indices);
        if (!On()) return;
        Rec& r = Next(Rec::Draw);
        Keep(r, s);
        r.a = prim, r.b = start, r.c = count;
        AsDrawn(r, s);
        r.headers.clear();
        for (const auto& stream : s.streams) AddHeader(r, stream.vb);
        for (uint32_t t : s.textures) AddHeader(r, t);
        KeepVertices(r, s, start, count, indices);
        for (uint32_t p : s.palettes) AddHeader(r, p);
        r.has_bytes = indices != nullptr;
        if (indices) r.bytes.assign(indices, indices + size_t(count) * 2);
    }
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t rects, uint32_t points) override {
        inner_->Copy(src, dst, count, rects, points);
        if (!On()) return;
        Rec& r = Next(Rec::Copy);
        r.s1 = src, r.s2 = dst, r.a = count, r.b = rects, r.c = points;
    }
    void Present(const Surface& front) override;
    void Sync(uint32_t va, uint32_t size) override { inner_->Sync(va, size); }
    void SyncVertices(uint32_t va) override { inner_->SyncVertices(va); }
    void SyncLate(uint32_t va) override { inner_->SyncLate(va); }
    bool DeliverLate(uint32_t va) override { return inner_->DeliverLate(va); }
    void RecordLate(uint32_t va) override { inner_->RecordLate(va); }
    void Flush() override { inner_->Flush(); }
    void Kick() override { inner_->Kick(); }
    void SetGamma(const uint8_t* ramp) override { inner_->SetGamma(ramp); }
    void ClearReport() override { inner_->ClearReport(); }
    void GetReport(uint32_t report) override { inner_->GetReport(report); }
    uint32_t Fence() override { return inner_->Fence(); }
    bool FencePending(uint32_t f) override { return inner_->FencePending(f); }
    void WaitFence(uint32_t f) override { inner_->WaitFence(f); }
    bool Busy(uint32_t r) override { return inner_->Busy(r); }
    void WaitResource(uint32_t r) override { inner_->WaitResource(r); }

private:
    static uint32_t Rate() {
        const uint32_t cap = app::Get().fps_cap;
        return cap ? cap : kFastest;
    }
    static bool On() { return app::kGame.step_interpolation && app::kGame.step_interpolation() && (Test() || Rate() > 60); }
    // headless: XBE_INTERP_TEST=N draws N frames a step, evenly, however long they take
    static int Test() {
        static const int n = [] {
            const char* e = std::getenv("XBE_INTERP_TEST");
            return e ? std::atoi(e) : 0;
        }();
        return n;
    }
    // vertex buffer, index buffer and palette headers are 12 bytes (as the render thread's copies)
    static uint32_t HeaderSize(uint32_t va) {
        const uint32_t type = kernel::R32(va) & 0x70000;
        return type < 0x20000 || type == 0x30000 ? 12 : kHeaderBytes;
    }
    static void AsDrawn(Rec& r, const State& s) {
        r.rt_format = s.rt.header ? kernel::R32(s.rt.header + 12) : 0;
        r.rt_size = s.rt.header ? kernel::R32(s.rt.header + 16) : 0;
        r.zs_format = s.zs.header ? kernel::R32(s.zs.header + 12) : 0;
        r.zs_size = s.zs.header ? kernel::R32(s.zs.header + 16) : 0;
    }
    // a replay draws into the targets the step drew into: their format and size as then, whatever the headers say by now
    static void UseAsDrawn(const Rec& r, State& d) {
        d.rt.format = r.rt_format, d.rt.size = r.rt_size, d.rt.as_drawn = d.rt.header != 0;
        d.zs.format = r.zs_format, d.zs.size = r.zs_size, d.zs.as_drawn = d.zs.header != 0;
    }
    // a constant past the matrix blended from the step before, unless it leapt or is a step of a cycle: Juiced's constants after
    // the transform pick an atlas cell (c33: 0, 0.25, 0.5, 0.75; the corner boards' arrows run through them), toggle each step
    // (c36.y 0.4 and 1) or tint one of many alike (the crowd's c42), and halfway between two of those is neither, the texture seen
    // flickering in every in-between frame (Harry, 2026-11-14). Those are the step's own; what moves smoothly (lights, the
    // bones) changes little in a step and is not a whole sixteenth twice
    static float Between(uint32_t k, float was, float now, float alpha) { return Holds(k, was, now) ? now : was + (now - was) * alpha; }
    // XBE_INTERP_CONST_LOG=1 (diagnosis): the constants blended past the transform, by program and register, logged every 10 s
    // with how often each changed, by how much, and the range it took (a flicker's constant: one that jumps about, blended)
    static bool ConstLog() {
        static const bool on = std::getenv("XBE_INTERP_CONST_LOG") != nullptr;
        return on;
    }
    struct ConstStat {
        uint64_t n = 0, back = 0;  // steps it changed in; of them, back to within 1% of the value two steps before
        double rel = 0;            // summed change over the larger magnitude
        float lo = 1e30f, hi = -1e30f, last_was = 0;
    };
    std::map<std::tuple<uint32_t, int, int>, ConstStat> const_stats_;  // program handle, register, component
    void NoteBlended(const Rec& r, const Rec& p) {
        for (const uint16_t e : r.lerp) {
            const uint32_t k = e >> 2, c = e & 3;
            if (k < kMatrixRegisters) continue;
            const float was = p.vsc[k][c], now = r.vsc[k][c];
            ConstStat& s = const_stats_[{r.s->vs_handle, int((k + Transform()) % kRegisters) - 96, int(c)}];
            const float m = std::max(std::fabs(was), std::fabs(now));
            s.back += s.n && std::fabs(now - s.last_was) <= 0.01f * std::max(m, 1e-6f);
            s.n++;
            s.rel += m > 0 ? std::fabs(now - was) / m : 0;
            s.lo = std::min({s.lo, was, now});
            s.hi = std::max({s.hi, was, now});
            s.last_was = was;
        }
    }
    void LogConstStats() {
        std::vector<std::pair<double, std::tuple<uint32_t, int, int>>> v;
        for (auto& [key, s] : const_stats_) v.push_back({s.rel, key});
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < v.size() && i < 40; i++) {
            const ConstStat& s = const_stats_[v[i].second];
            const auto [vs, reg, c] = v[i].second;
            LOGR("gpu: in-between constants: vs %08X c%d.%c changed %llu times (back %llu), mean change %.3f, range %g to %g", vs, reg,
                 "xyzw"[c], (unsigned long long)s.n, (unsigned long long)s.back, s.rel / double(s.n), s.lo, s.hi);
        }
        const_stats_.clear();
    }
    // the step's own value taken, not blended
    static bool Holds(uint32_t k, float was, float now) {
        // XBE_INTERP_CONSTANTS=0 (a test): every constant past the transform at the step's value
        static const bool constants = [] {
            const char* e = std::getenv("XBE_INTERP_CONSTANTS");
            return !e || std::atoi(e) != 0;
        }();
        if (k < kMatrixRegisters) return false;
        if (!constants) return true;
        if (app::kGame.interp_blends_constant &&
            !app::kGame.interp_blends_constant(int((k + Transform()) % kRegisters) - 96, was, now))
            return true;
        if (was != now) {
            const float d = std::fabs(now - was);
            if (d >= kConstantLeap && d >= 0.5f * std::max(std::fabs(was), std::fabs(now))) return true;
            const auto cell = [](float v) { return std::fabs(v) <= 16 && v * 16 == std::floor(v * 16); };
            if (cell(was) && cell(now)) return true;
        }
        return false;
    }
    static void AddHeader(Rec& r, uint32_t va) {
        // a stream still bound to a buffer the title has freed (Juiced, as a race loads): its memory is gone, and no
        // draw reads it (gpu::State keeps the binding as the device does)
        if (!va || !mem::IsCommitted(va) || !mem::IsCommitted(va + kHeaderBytes - 1)) return;
        for (const Rec::Header& h : r.headers)
            if (h.va == va) return;
        Rec::Header h{va, {}};
        std::memcpy(h.bytes, kernel::HostPtr(va), HeaderSize(va));
        r.headers.push_back(h);
    }
    void KeepVertices(Rec& r, const State& s, uint32_t start, uint32_t count, const uint8_t* indices);
    bool UploadKept();
    void LogJumps();
    Rec& Next(Rec::Kind kind) {
        if (!count_) first_ = Clock::now();
        if (count_ == now_.size()) now_.emplace_back();
        Rec& r = now_[count_++];
        r.kind = kind;
        r.prev = -1;
        r.turning = false;
        r.derived = false;
        r.stray = false;
        r.peer = -1;
        r.headers.clear();
        r.kept.clear();
        return r;
    }
    static void Keep(Rec& r, const State& s) {
        if (!r.s) r.s = std::make_unique<State>();
        State& d = *r.s;
        // the fixed function matrices are most of it: only a draw without a program reads them
        if (!(s.vs_handle & 1)) {
            d = s;
        } else {
            const auto* from = reinterpret_cast<const uint8_t*>(&s);
            auto* to = reinterpret_cast<uint8_t*>(&d);
            const size_t vs = size_t(reinterpret_cast<const uint8_t*>(&s.vs) - from);
            const size_t vsc = size_t(reinterpret_cast<const uint8_t*>(&s.vsc) - from);
            const size_t transforms = size_t(reinterpret_cast<const uint8_t*>(&s.transforms) - from);
            const size_t material = size_t(reinterpret_cast<const uint8_t*>(&s.material) - from);
            std::memcpy(to, from, vs);
            d.vs = s.vs;
            d.ps = s.ps;
            std::memcpy(to + vsc, from + vsc, transforms - vsc);
            std::memcpy(to + material, from + material, sizeof(State) - material);
        }
        for (uint32_t k = 0; k < kRegisters; k++) r.vsc[k] = s.vsc[(k + Transform()) % kRegisters];
    }
    // finds each draw's last self; false when there is nothing to blend or the camera was cut
    bool Match();
    void Replay(float alpha);
    uint32_t steps_seen_ = 1;  // counts every step, for Rec::lerp_step
    void FindStillTargets();
    std::vector<uint8_t> skip_;  // records left out of the replays (FindStillTargets), for skip_step_
    uint32_t skip_step_ = 0, front_data_ = 0;
    std::unordered_set<uint32_t> camera_targets_;  // the targets with a camera move this step (Match)
    uint64_t sum_skipped_ = 0;
    double sum_draw_calls_ = 0;  // XBE_PERF: replays' time in the renderer's Draw

    std::unique_ptr<Backend> inner_;
    std::vector<Rec> now_, before_;
    size_t count_ = 0, before_count_ = 0;
    std::vector<uint8_t> kept_bytes_;           // this step's kept vertices
    uint32_t kept_count_ = 0;
    bool kept_full_ = false;
    uint32_t region_ = 0, region_data_[2] = {}, region_headers_[2] = {}, region_fence_[2] = {};
    Clock::time_point presented_{}, first_{};
    double replay_seconds_ = 0, typical_ = 0, frame_seconds_ = 0;
    int limit_ = kMostFrames, calm_ = 0, patience_ = kFirstPatience, late_ = 0;
    bool raised_ = false;
    bool overran_ = false;  // the last step's frames went on past the time they had
    uint32_t late_ours_ = 0, late_title_ = 0;
    Clock::time_point rest_until_{};
    bool logged_ = false;
    Clock::time_point reported_{};
    uint32_t shown_ = 0, steps_ = 0, blended_steps_ = 0;
    // what holds the frame rate, summed over the 10 s summary: the title's own time a step (its records to its present), the match
    // and upload before the replays, the replays, the frames planned and the limit the late steps leave
    double sum_drawing_ = 0, sum_prepare_ = 0, sum_replay_ = 0, sum_planned_ = 0, sum_limit_ = 0;
    uint32_t replays_ = 0;
};

void Interpolating::KeepVertices(Rec& r, const State& s, uint32_t start, uint32_t count, const uint8_t* indices) {
    uint32_t vertices = start + count;
    if (indices) {
        uint16_t most = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint16_t v;
            std::memcpy(&v, indices + size_t(i) * 2, 2);
            most = std::max(most, v);
        }
        vertices = uint32_t(most) + 1 + s.base_vertex;
    }
    for (uint32_t k = 0; k < s.streams.size(); k++) {
        const auto& st = s.streams[k];
        if (!st.vb || !st.stride || !mem::IsCommitted(st.vb + 7)) continue;
        const uint64_t bytes = uint64_t(vertices) * st.stride;
        if (bytes > kCopyBytes) continue;
        const uint32_t data = kernel::R32(st.vb + 4);
        if (!data) continue;
        const uint32_t va = mem::VirtualFromPhysical(data);
        if (!mem::IsCommitted(va) || !mem::IsCommitted(va + uint32_t(bytes) - 1)) continue;
        if (kept_bytes_.size() + bytes > kRegionBytes || kept_count_ >= kRegionHeaders) {
            kept_full_ = true;  // that draw would read what the title wrote after it: the last draws, the HUD, in pieces
            continue;
        }
        kept_count_++;
        const auto* from = static_cast<const uint8_t*>(kernel::HostPtr(va));
        r.kept.push_back({uint8_t(k), uint32_t(kept_bytes_.size()), uint32_t(bytes)});
        kept_bytes_.insert(kept_bytes_.end(), from, from + bytes);
    }
}

// into memory the renderers can read, with a vertex buffer header for each; the region the step before used may still be being read
bool Interpolating::UploadKept() {
    region_ ^= 1;
    if (!region_data_[region_]) {
        region_data_[region_] = mem::AllocHostContiguous(kRegionBytes, 0x1000);
        region_headers_[region_] = mem::AllocHostContiguous(kRegionHeaders * 12, 0x1000);
    }
    if (!region_data_[region_] || !region_headers_[region_]) return false;
    inner_->WaitFence(region_fence_[region_]);
    if (!kept_bytes_.empty()) std::memcpy(kernel::HostPtr(region_data_[region_]), kept_bytes_.data(), kept_bytes_.size());
    return true;
}

// to the time, not past it: a sleep on Windows wakes 1 to 2 ms late even at a 1 ms timer period, and the next frame of a step,
// timed to be done 1 ms before the title goes on, missed it and was dropped about one step in two (Harry's 120 cap: 880
// frames for 600 steps, 2 planned; the picture moved unevenly, the arrow boards and the crowd seen to flicker). The last of it
// waited out
constexpr auto kSpinTail = std::chrono::microseconds(1500);
void SleepUntil(Clock::time_point when) {
    if (Clock::now() < when - kSpinTail) std::this_thread::sleep_until(when - kSpinTail);
    while (Clock::now() < when) std::this_thread::yield();
}

// 4 by 4, rows; false when it has no inverse
bool Invert(const double m[4][4], double out[4][4]) {
    double a[4][8];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[i][j] : (j - 4 == i ? 1 : 0);
    for (int c = 0; c < 4; c++) {
        int best = c;
        for (int i = c + 1; i < 4; i++)
            if (std::fabs(a[i][c]) > std::fabs(a[best][c])) best = i;
        if (std::fabs(a[best][c]) < 1e-12) return false;
        for (int j = 0; j < 8; j++) std::swap(a[c][j], a[best][j]);
        const double d = a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] /= d;
        for (int i = 0; i < 4; i++) {
            if (i == c) continue;
            const double f = a[i][c];
            for (int j = 0; j < 8; j++) a[i][j] -= f * a[c][j];
        }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out[i][j] = a[i][j + 4];
    return true;
}

// XBE_INTERP_LOG=1: draws at a car's distance whose place on screen jumps between the two steps blended, for what cannot be seen here
void Interpolating::LogJumps() {
    static const bool on = std::getenv("XBE_INTERP_LOG") != nullptr;
    if (!on) return;
    // each kind of draw a few times, in all at most a line a step: what is rare is what is wanted
    static std::unordered_map<uint64_t, int> kinds;
    static int shown = 0, steps = 0;
    if (++steps % 600 == 0) shown = 0, kinds.clear();
    const auto place = [](const float m[4][4], float* o) {
        const float w = m[3][3];
        if (std::fabs(w) < 1.5f || w > 60) return false;  // nearer is scenery passing the camera, moving far on screen as it should
        o[0] = m[0][3] / w, o[1] = m[1][3] / w, o[2] = w;
        return true;
    };
    // constants past the matrix that change far in a step (a scroll wrapping, a counter): blended, they sweep the whole range
    static std::unordered_map<uint64_t, int> wraps;
    for (size_t i = 0; i < count_; i++) {
        const Rec& r = now_[i];
        if (r.kind != Rec::Draw || r.prev < 0) continue;
        const Rec& p = before_[size_t(r.prev)];
        for (uint32_t k = kMatrixRegisters; k < kRegisters; k++)
            for (uint32_t c = 0; c < 4; c++) {
                const float x = p.vsc[k][c], y = r.vsc[k][c], d = std::fabs(x - y);
                if (d < kConstantLeap || d < 0.5f * std::max(std::fabs(x), std::fabs(y))) continue;
                if (++wraps[(uint64_t(r.s->vs_handle) << 16) ^ (k * 4 + c)] > 3) continue;
                LOGR("wrap: step %d vs %08X t0 %08X n %u c%d.%c %.4f -> %.4f", steps, r.s->vs_handle, r.s->textures[0], r.c,
                     int((k + Transform()) % kRegisters) - 96,
                     "xyzw"[c], x, y);
            }
    }
    for (size_t i = 0; i < count_ && shown < 200; i++) {
        const Rec& r = now_[i];
        if (r.kind != Rec::Draw || std::fabs(r.vsc[3][0]) + std::fabs(r.vsc[3][1]) + std::fabs(r.vsc[3][2]) < 1e-6f) continue;
        float a[4][4], b[4][4];
        for (size_t x = 0; x < 4; x++)
            for (size_t y = 0; y < 4; y++) a[x][y] = r.vsc[x][y];
        const char* how;
        if (r.prev >= 0) {
            how = r.turning ? "turning" : "matched";
            for (size_t x = 0; x < 4; x++)
                for (size_t y = 0; y < 4; y++) b[x][y] = before_[size_t(r.prev)].vsc[x][y];
        } else if (r.derived) {
            how = "piece";
            std::memcpy(b, r.was, sizeof b);
        } else {
            continue;
        }
        float p[3], q[3];
        if (!place(a, p) || !place(b, q)) continue;
        const float d = std::max(std::fabs(p[0] - q[0]), std::fabs(p[1] - q[1])), wr = std::fabs(q[2] / p[2] - 1);
        if (d < 0.04f && wr < 0.04f) continue;
        if (++kinds[(uint64_t(r.s->vs_handle) << 32) ^ r.s->textures[0] ^ (uint64_t(r.c) << 12)] > 3) continue;
        shown++;
        LOGR("jump: %s step %d vs %08X t0 %08X n %u rt %08X moved %.3f depth %.3f w %.2f was %.2f", how, steps, r.s->vs_handle,
             r.s->textures[0], r.c, r.s->rt.data, d, wr, p[2], q[2]);
    }
}

bool Interpolating::Match() {
    if (!before_count_) return false;
    struct Move {
        uint32_t rt;
        double d[4][4];
        int votes;
    };
    const auto flat = [](const std::array<std::array<float, 4>, 192>& v) {  // a screen's 2D has no perspective row
        return std::fabs(v[3][0]) + std::fabs(v[3][1]) + std::fabs(v[3][2]) < 1e-6f;
    };
    const auto move_of = [&](const Rec& r, double d[4][4]) {
        double n[4][4], inv[4][4];
        const auto& p = before_[size_t(r.prev)].vsc;
        for (int a = 0; a < 4; a++)
            for (int b = 0; b < 4; b++) n[a][b] = r.vsc[size_t(a)][size_t(b)];
        if (!Invert(n, inv)) return false;
        for (int a = 0; a < 4; a++)
            for (int b = 0; b < 4; b++) {
                d[a][b] = 0;
                for (int c = 0; c < 4; c++) d[a][b] += double(p[size_t(a)][size_t(c)]) * inv[c][b];
            }
        return true;
    };
    // a step's moves are all small: alike is judged against how far the camera went, or a car would pass for scenery
    const auto same_move = [](const double x[4][4], const double y[4][4]) {
        double e = 0, size = 0;
        for (int a = 0; a < 4; a++)
            for (int b = 0; b < 4; b++) {
                e += std::fabs(x[a][b] - y[a][b]);
                size += std::fabs(y[a][b] - (a == b ? 1 : 0));
            }
        return e < std::max(kSameMove * size, kStill);
    };
    // each draw of the step before where the camera's move would put it now (its target's camera taken back off it), or as it was
    std::vector<std::array<float, 16>> predicted(before_count_);
    for (size_t i = 0; i < before_count_; i++)
        for (uint32_t k = 0; k < kMatrixRegisters; k++)
            for (uint32_t e = 0; e < 4; e++) predicted[i][k * 4 + e] = before_[i].vsc[k][e];
    uint32_t draws = 0, matched = 0, moved = 0, jumped = 0, strays = 0;
    bool predicting = false;  // predicted holds the camera's move taken off
    // programs whose matched draws mostly go with the camera (still scenery, the crowd): their copies are told apart by where the
    // camera's move puts them. Not the cars': alike, and each moving its own way
    std::unordered_map<uint32_t, bool> still;
    const auto match_all = [&] {
        std::unordered_map<uint64_t, std::vector<int>> found;
        found.reserve(before_count_);
        for (size_t i = before_count_; i-- > 0;)
            if (before_[i].kind == Rec::Draw) found[KeyOf(before_[i])].push_back(int(i));  // popped from the back: in order
        draws = matched = moved = jumped = strays = 0;
        for (size_t i = 0; i < count_; i++) {
            Rec& r = now_[i];
            if (r.kind != Rec::Draw) continue;
            r.prev = -1;
            r.turning = false;
            r.stray = false;
            draws++;
            auto it = found.find(KeyOf(r));
            if (it == found.end() || it->second.empty()) continue;
            // of those alike (a mesh drawn in many places, a piece of the HUD), the one whose matrix, moved as the camera moved, is
            // nearest: by the matrices alone a crowd, a row of lamps or a street's repeated fronts took a neighbour's, and the
            // in-between frames drew it part way to the neighbour's place (Harry's clip of 3f50b78, 2026-11-14)
            auto& list = it->second;
            size_t best = list.size() - 1;
            float best_distance = 1e30f, second = 1e30f;
            for (size_t l = list.size(); l-- > 0;) {
                const auto& c = predicted[size_t(list[l])];
                float distance = 0;
                for (uint32_t k = 0; k < kMatrixRegisters; k++)
                    for (uint32_t e = 0; e < 4; e++) distance += std::fabs(c[k * 4 + e] - r.vsc[k][e]);
                if (distance < best_distance) second = best_distance, best_distance = distance, best = l;
                else if (distance < second) second = distance;
            }
            const int candidate = list[best];
            // where the camera's move puts it, the right copy is where it is now, but for its own move: one come into view this step
            // took a copy elsewhere, a screen or more away (Juiced's street, 2 to 5 screens in the log). It has no self to blend from
            // only among copies: a draw with one self (a car crossing the screen in the flyover) is that self however it moved
            // and only when another copy is nearly as near: a copy that moves (a car's wheel in the flyover) is still far nearer its
            // self than any other
            if (predicting && list.size() > 1 && second < 2 * best_distance && !flat(r.vsc) && still.count(r.s->vs_handle)) {
                // on the screen in its own widths: x / w over the projection's scale, the row's length against the w row's
                const auto len = [](float a, float b, float c) { return std::sqrt(a * a + b * b + c * c); };
                const auto& c = predicted[size_t(candidate)];
                const float w0 = c[15], w1 = r.vsc[3][3], sw = len(r.vsc[3][0], r.vsc[3][1], r.vsc[3][2]);
                const float sx = len(r.vsc[0][0], r.vsc[0][1], r.vsc[0][2]) / sw, sy = len(r.vsc[1][0], r.vsc[1][1], r.vsc[1][2]) / sw;
                if (sw > 0 && sx > 0 && sy > 0 && std::fabs(w0) > 1 && std::fabs(w1) > 1 &&
                    (std::fabs(c[3] / w0 - r.vsc[0][3] / w1) > kStray * sx || std::fabs(c[7] / w0 - r.vsc[1][3] / w1) > kStray * sy ||
                     std::fabs(w0 / w1 - 1) > kStrayDepth)) {
                    strays++;
                    r.stray = true;
                    continue;
                }
            }
            list.erase(list.begin() + std::ptrdiff_t(best));
            const Rec& was = before_[size_t(candidate)];
            // by the matrix's turn and scale, each row against its own size: its translation crosses zero as an object is passed, a
            // small number with a large change, and the object is the same. Past that only a real leap in it counts
            bool turned = false, leapt = false;
            for (uint32_t k = 0; k < kMatrixRegisters; k++) {
                float size = 0;
                for (uint32_t c = 0; c < 3; c++) size = std::max({size, std::fabs(was.vsc[k][c]), std::fabs(r.vsc[k][c])});
                for (uint32_t c = 0; c < 3; c++)
                    if (std::fabs(was.vsc[k][c] - r.vsc[k][c]) > kFar * size) turned = true;
                const float x = was.vsc[k][3], y = r.vsc[k][3];
                // in the matrix's own units: the world's rows are near 1, a screen's HUD 0.003 a pixel, across the screen is a leap there
                if (std::fabs(x - y) > kLeap * size && std::fabs(x - y) > kFar * std::max(std::fabs(x), std::fabs(y))) leapt = true;
            }
            matched++;
            if (leapt) {  // a different draw, alike in all but place
                jumped++;
                continue;
            }
            r.turning = turned;  // a wheel or a piece of debris turning fast stays with what carries it: only its place is blended
            r.prev = candidate;
            if (std::memcmp(before_[size_t(r.prev)].vsc.data(), r.vsc.data(), sizeof r.vsc)) moved++;
        }
    };
    // the camera's move for each target, the one most matched draws agree on (step before = move * now)
    std::vector<Move> moves;
    std::unordered_map<uint32_t, bool> spinning;
    const auto find_moves = [&] {
        moves.clear();
        spinning.clear();
        for (size_t i = 0, tried = 0; i < count_; i++) {
            const Rec& r = now_[i];
            if (r.kind != Rec::Draw || r.prev < 0 || flat(r.vsc)) continue;
            if (r.turning) spinning[r.s->vs_handle] = true;
            if (r.turning || tried >= kSampleDraws) continue;
            tried++;
            Move m{r.s->rt.data, {}, 1};
            if (!move_of(r, m.d)) continue;
            bool same = false;
            for (Move& o : moves)
                if (o.rt == m.rt && same_move(o.d, m.d)) {
                    o.votes++;
                    same = true;
                    break;
                }
            if (!same && moves.size() < kMostMoves) moves.push_back(m);
        }
    };
    const auto camera_of = [&](uint32_t rt) {
        const Move* camera = nullptr;
        for (const Move& o : moves)
            if (o.rt == rt && (!camera || o.votes > camera->votes)) camera = &o;
        return camera;
    };
    match_all();
    find_moves();
    // matched again, with the camera's move from the first matching taken back off the step before's matrices
    static const bool predict = [] {
        const char* e = std::getenv("XBE_INTERP_PREDICT");
        return !e || std::atoi(e) != 0;
    }();
    if (predict) {
        std::unordered_map<uint32_t, std::array<double, 16>> undo;
        for (const Move& m : moves) {
            if (undo.count(m.rt)) continue;
            const Move* camera = camera_of(m.rt);
            double inv[4][4];
            if (!camera || camera->votes < kLeastVotes || !Invert(camera->d, inv)) continue;
            auto& u = undo[m.rt];
            for (int a = 0; a < 4; a++)
                for (int b = 0; b < 4; b++) u[size_t(a * 4 + b)] = inv[a][b];
        }
        if (!undo.empty()) {
            for (size_t i = 0; i < before_count_; i++) {
                const Rec& p = before_[i];
                if (p.kind != Rec::Draw || flat(p.vsc)) continue;
                const auto u = undo.find(p.s->rt.data);
                if (u == undo.end()) continue;
                for (uint32_t a = 0; a < 4; a++)
                    for (uint32_t b = 0; b < 4; b++) {
                        double t = 0;
                        for (uint32_t c = 0; c < 4; c++) t += u->second[a * 4 + c] * p.vsc[c][b];
                        predicted[i][a * 4 + b] = float(t);
                    }
            }
            std::unordered_map<uint32_t, std::array<int, 2>> share;
            for (size_t i = 0; i < count_; i++) {
                const Rec& r = now_[i];
                if (r.kind != Rec::Draw || r.prev < 0 || r.turning || flat(r.vsc)) continue;
                const Move* camera = camera_of(r.s->rt.data);
                double d[4][4];
                if (!camera || !move_of(r, d)) continue;
                auto& e = share[r.s->vs_handle];
                e[0]++;
                if (same_move(d, camera->d)) e[1]++;
            }
            for (const auto& [vs, e] : share)
                if (e[1] * 2 > e[0]) still[vs] = true;
            predicting = true;
            match_all();
            find_moves();
        }
    }
    // XBE_INTERP_CAMERA_ONLY=1 (a test): every draw of the 3D takes its target's camera move, none its own match; cars and
    // what turns go wrong, but whatever flickers with matching and blending of the draws' own matrices does not
    // 2 (GameInfo::interp_scenery_takes_camera): only the draws of still programs (most of whose matched draws go with the camera): the scenery, which
    // does not move but with the camera, and each of whose draws blended from its own match, or left at the step's place,
    // or a sprite, or a second pass, could be a little off it, the buildings shaking (Harry, 2026-11-15: steady only like
    // this). Its constants are the step's own (the corner boards' arrow frames). 3: only the others'; 0: none
    static const int camera_only = [] {
        const char* e = std::getenv("XBE_INTERP_CAMERA_ONLY");
        const int on = e && *e ? std::atoi(e) : app::kGame.interp_scenery_takes_camera ? 2 : 0;
        if (on) LOGR("gpu: in-between frames: draws take their target's camera move (XBE_INTERP_CAMERA_ONLY=%d)", on);
        return on;
    }();
    // a still program's draw sharing its matrix with another program's goes with that: a car's brake lights, drawn by the
    // street lamps' glow program over the car's body (given the camera's move they were left behind the car, unlit in every
    // in-between frame; Harry, 2026-11-15)
    std::unordered_set<uint64_t> moving;
    const auto matrix_hash = [](const Rec& r) {
        uint64_t h = 0xCBF29CE484222325ull;
        const auto* p = reinterpret_cast<const uint8_t*>(r.vsc.data());
        for (size_t b = 0; b < kMatrixRegisters * sizeof(r.vsc[0]); b++) h = (h ^ p[b]) * 0x100000001B3ull;
        return h;
    };
    if (camera_only == 2)
        for (size_t i = 0; i < count_; i++)
            if (now_[i].kind == Rec::Draw && !flat(now_[i].vsc) && !still.count(now_[i].s->vs_handle)) moving.insert(matrix_hash(now_[i]));
    if (camera_only)
        for (size_t i = 0; i < count_; i++) {
            Rec& r = now_[i];
            if (r.kind != Rec::Draw || flat(r.vsc)) continue;
            if (camera_only == 2 && (!still.count(r.s->vs_handle) || moving.count(matrix_hash(r)))) continue;
            if (camera_only == 3 && still.count(r.s->vs_handle)) continue;
            const Move* camera = camera_of(r.s->rt.data);
            if (!camera || camera->votes < kLeastVotes) continue;
            for (int a = 0; a < 4; a++)
                for (int b = 0; b < 4; b++) {
                    double t = 0;
                    for (int c = 0; c < 4; c++) t += camera->d[a][c] * r.vsc[size_t(c)][size_t(b)];
                    r.was[a][b] = float(t);
                }
            r.prev = -1, r.peer = -1, r.turning = false, r.derived = true;
        }
    // a still scenery program's draw whose own move puts its in-between place away from where the camera's move does is matched
    // to the wrong self (one of a street's repeated fronts): blended from it, a building slid away from the road and back
    // every step on a corner (Harry's clip of a1979db, 2026-11-15; headless, one in ten of those off the camera's move by 1%
    // of the screen or more). It takes the camera's move, as scenery come into view does. Before peers are found: a peer
    // follows its matched draw's match. XBE_INTERP_OFF_CAMERA=0 leaves them blended from their own match
    static const bool off_camera = [] {
        const char* e = std::getenv("XBE_INTERP_OFF_CAMERA");
        return !e || std::atoi(e) != 0;
    }();
    uint32_t refit = 0;
    if (off_camera && predicting)
        for (size_t i = 0; i < count_; i++) {
            Rec& r = now_[i];
            if (r.kind != Rec::Draw || r.prev < 0 || r.turning || flat(r.vsc) || !still.count(r.s->vs_handle)) continue;
            const Move* camera = camera_of(r.s->rt.data);
            double d[4][4];
            if (!camera || camera->votes < kLeastVotes || !move_of(r, d) || same_move(d, camera->d)) continue;
            // its place halfway, on the screen in its own widths (x / w over the projection's scale), both ways
            double was[4][4];
            for (int a = 0; a < 4; a++)
                for (int b = 0; b < 4; b++) {
                    was[a][b] = 0;
                    for (int c = 0; c < 4; c++) was[a][b] += camera->d[a][c] * r.vsc[size_t(c)][size_t(b)];
                }
            const auto& p = before_[size_t(r.prev)].vsc;
            const double ox = (p[0][3] + r.vsc[0][3]) / 2, ow = (p[3][3] + r.vsc[3][3]) / 2;
            const double cx = (was[0][3] + r.vsc[0][3]) / 2, cw = (was[3][3] + r.vsc[3][3]) / 2;
            const auto len = [](double a, double b, double c) { return std::sqrt(a * a + b * b + c * c); };
            const double sw = len(r.vsc[3][0], r.vsc[3][1], r.vsc[3][2]);
            const double sx = sw > 0 ? len(r.vsc[0][0], r.vsc[0][1], r.vsc[0][2]) / sw : 0;
            if (!(std::fabs(ow) > 1 && std::fabs(cw) > 1 && sx > 0) || std::fabs(ox / ow - cx / cw) / sx <= kOffCamera) continue;
            for (int a = 0; a < 4; a++)
                for (int b = 0; b < 4; b++) r.was[a][b] = float(was[a][b]);
            r.prev = -1, r.derived = true;
            refit++;
        }
    {
        static const bool log = std::getenv("XBE_INTERP_LOG") != nullptr;
        static uint32_t sum = 0, steps = 0;
        sum += refit;
        if (log && ++steps % 600 == 0) {
            LOGR("gpu: in-between frames: %u scenery draws off the camera's move given it in 600 steps", sum);
            sum = 0;
        }
    }
    {
        // XBE_INTERP_LOG: matched draws of the 3D whose own move is not their target's camera's (a car, or a wrong match)
        static const bool log = std::getenv("XBE_INTERP_LOG") != nullptr;
        static uint32_t off = 0, all = 0, steps = 0, stray_sum = 0;
        stray_sum += strays;
        if (log) {
            for (size_t i = 0; i < count_; i++) {
                const Rec& r = now_[i];
                if (r.kind != Rec::Draw || r.prev < 0 || r.turning || flat(r.vsc)) continue;
                const Move* camera = camera_of(r.s->rt.data);
                double d[4][4];
                if (!camera || !move_of(r, d)) continue;
                all++;
                if (!same_move(d, camera->d)) off++;
            }
            if (++steps % 600 == 0) {
                LOGR("gpu: in-between frames: %u of %u matched draws not moving with the camera in 600 steps, %u strays", off, all, stray_sum);
                off = all = stray_sum = 0;
            }
        }
    }
    // the world's pieces come and go between steps. One left at the step's own place, among others blended, is that much out of
    // line with them, its texture seen sliding: it goes with any of the same matrix that has a match
    std::unordered_map<uint64_t, int> peers;
    const auto matrix_key = [](const Rec& r) {
        uint64_t h = 0xCBF29CE484222325ull;
        const auto* p = reinterpret_cast<const uint8_t*>(r.vsc.data());
        for (size_t b = 0; b < kMatrixRegisters * sizeof(r.vsc[0]); b++) h = (h ^ p[b]) * 0x100000001B3ull;
        return h;
    };
    for (size_t i = 0; i < count_; i++)
        if (now_[i].kind == Rec::Draw && now_[i].prev >= 0 && !now_[i].turning) peers.emplace(matrix_key(now_[i]), int(i));
    uint32_t followed = 0;
    for (size_t i = 0; i < count_; i++) {
        Rec& r = now_[i];
        if (r.kind != Rec::Draw || r.prev >= 0) continue;
        if (auto it = peers.find(matrix_key(r)); it != peers.end()) {
            r.peer = it->second;
            followed++;
        }
    }
    {
        static const bool log_lost = std::getenv("XBE_INTERP_LOG") != nullptr;
        static uint32_t all_lost = 0, most = 0, steps = 0;
        uint32_t lost = 0;
        for (size_t i = 0; i < count_; i++)
            if (now_[i].kind == Rec::Draw && now_[i].prev < 0) lost++;
        all_lost += lost, most = std::max(most, lost);
        if (log_lost && ++steps % 600 == 0) {
            LOGR("gpu: in-between frames: %u draws without a match in 600 steps, at most %u in a step", all_lost, most);
            all_lost = most = 0;
        }
    }
    static const bool pieces = [] {
        const char* e = std::getenv("XBE_INTERP_PIECES");
        const bool on = !e || std::atoi(e) != 0;
        LOGR("gpu: in-between frames: scenery come into view %s (XBE_INTERP_PIECES)", on ? "takes the camera's move" : "stays at its step's place");
        return on;
    }();
    // A piece of scenery come into view this step has no self to blend from, and its own matrix. Left at the step's place while what is
    // around it runs behind, it is seen to jump (a building's lit windows). It takes the camera's move, step before = move * now, read
    // off matched draws of the same target, the one most agree on. Only scenery: the nearest matched draw of its program must move
    // that way and nothing of that program be turning. A piece of a car given a wheel's move, or the road's, is thrown out of place;
    // it stays at the step's place.
    {
        // a program is the scenery's when most of what it draws goes with the camera; a car's goes with the car
        std::unordered_map<uint32_t, std::array<int, 2>> program;
        for (size_t i = 0; i < count_; i++) {
            const Rec& r = now_[i];
            if (r.kind != Rec::Draw || r.prev < 0 || flat(r.vsc)) continue;
            const Move* camera = camera_of(r.s->rt.data);
            double d[4][4];
            if (!camera || !move_of(r, d)) continue;
            auto& seen = program[r.s->vs_handle];
            seen[0]++;
            if (same_move(d, camera->d)) seen[1]++;
        }
        // scenery comes into view a piece at a time among many that are matched. A whole car losing its match for a step comes all at
        // once, and its program has as many new as matched: not scenery
        std::unordered_map<uint32_t, int> fresh;
        static const bool all_scenery = [] {  // XBE_INTERP_SCENERY=0: only scenery the checks below let through, as before
            const char* e = std::getenv("XBE_INTERP_SCENERY");
            return !e || std::atoi(e) != 0;
        }();
        for (size_t i = 0; i < count_; i++)
            if (now_[i].kind == Rec::Draw && now_[i].prev < 0 && !now_[i].derived) fresh[now_[i].s->vs_handle]++;
        for (size_t i = 0; i < count_; i++) {
            Rec& r = now_[i];
            // a few vertices are a sprite, a plate or a car's shadow, on the car and not the street, whatever program draws them
            if (!pieces || r.kind != Rec::Draw || r.prev >= 0 || r.derived || flat(r.vsc) || spinning.count(r.s->vs_handle)) continue;
            // of a program whose matched draws go with the camera (still scenery), any piece without a self: left at the step's
            // place, ten a step of them off the camera's by 1% of the screen or more near the race, a building slid away from the
            // road and back (Harry's clip of a1979db; all of them given the camera's move, XBE_INTERP_CAMERA_ONLY, it was gone)
            // not one following a matched draw of its own matrix (a peer): a second pass over the same surface (the corner boards'
            // arrows over the board) given the camera's move instead lay a little off the board's own blend and fought with it
            static const bool with_peers = [] {  // XBE_INTERP_SCENERY_PEERS=1 (a test): peers too
                const char* e = std::getenv("XBE_INTERP_SCENERY_PEERS");
                return e && std::atoi(e) != 0;
            }();
            static const bool with_sprites = [] {  // XBE_INTERP_SCENERY_SPRITES=1 (a test): sprites too
                const char* e = std::getenv("XBE_INTERP_SCENERY_SPRITES");
                return e && std::atoi(e) != 0;
            }();
            const bool scenery = all_scenery && (r.peer < 0 || with_peers) && still.count(r.s->vs_handle);
            // and a few vertices are a sprite whatever its program: the corner boards' arrows, small quads, given the camera's move
            // broke again (Harry, 271295c and 40fc432)
            if (r.c < kLeastVertices && !r.stray && !(scenery && with_sprites)) continue;
            const Move* camera = camera_of(r.s->rt.data);
            if (!camera || camera->votes < kLeastVotes) continue;
            const auto seen = program.find(r.s->vs_handle);
            if (!scenery && (seen == program.end() || seen->second[1] * 2 <= seen->second[0] || seen->second[1] < fresh[r.s->vs_handle] * kFreshShare)) continue;
            const Rec* near = nullptr;
            float nearest = 1e30f;
            for (size_t j = 0; !scenery && j < count_; j++) {
                const Rec& o = now_[j];
                if (o.kind != Rec::Draw || o.prev < 0 || o.s->vs_handle != r.s->vs_handle || o.s->rt.data != r.s->rt.data) continue;
                float distance = 0;
                for (uint32_t k = 0; k < kMatrixRegisters; k++)
                    for (uint32_t c = 0; c < 4; c++) distance += std::fabs(o.vsc[k][c] - r.vsc[k][c]);
                if (distance < nearest) nearest = distance, near = &o;
            }
            double d[4][4];
            if (!scenery && (!near || !move_of(*near, d) || !same_move(d, camera->d))) continue;
            for (int a = 0; a < 4; a++)
                for (int b = 0; b < 4; b++) {
                    double t = 0;
                    for (int c = 0; c < 4; c++) t += camera->d[a][c] * r.vsc[size_t(c)][size_t(b)];
                    r.was[a][b] = float(t);
                }
            r.derived = true;
        }
    }
    camera_targets_.clear();
    for (const Move& m : moves)
        if (m.votes >= kLeastVotes) camera_targets_.insert(m.rt & 0x0FFFFFFF);
    LogJumps();
    if (!moved || !(matched - jumped)) return false;
    if (jumped * 2 > matched) return false;  // a camera cut
    return true;
}

void Interpolating::Replay(float alpha) {
    const auto began = Clock::now();
    // diagnosis: a file named interp_table in the working folder (the run folder headless) logs the next 8 replays' draws and how
    // each was blended, to pair with the reference renderer's probes (PROBES, which name each writing draw's program, count and
    // textures) on a pixel that flickers
    static int diag_left = 0, diag_replay = 0;
    diag_replay++;
    if (!diag_left) {
        if (std::FILE* f = std::fopen("interp_table", "rb")) {
            std::fclose(f);
            std::remove("interp_table");
            diag_left = 8;
        }
    }
    const bool diag_table = diag_left > 0;
    if (diag_table) diag_left--;
    if (diag_table) LOGR("gpu: in-between table: replay %d alpha %.2f records %zu", diag_replay, alpha, count_);
    // GameInfo::interp_sampled_targets_at_step: the memory of targets drawn this step by draws that are not blended (no match,
    // or a leap); a draw sampling one of them is drawn at the step's place, as what it samples is
    std::vector<uint32_t> unblended_targets;
    if (alpha != 1 && app::kGame.interp_sampled_targets_at_step)
        for (size_t i = 0; i < count_; i++) {
            const Rec& r = now_[i];
            if (r.kind != Rec::Draw || r.prev >= 0 || r.derived || r.peer >= 0 || !r.s->rt.data) continue;
            const uint32_t d = r.s->rt.data & 0x0FFFFFFF;
            if (std::find(unblended_targets.begin(), unblended_targets.end(), d) == unblended_targets.end()) unblended_targets.push_back(d);
        }
    const auto samples_unblended = [&](const State& st) {
        for (uint32_t t : st.textures)
            if (t && std::find(unblended_targets.begin(), unblended_targets.end(), kernel::R32(t + 4) & 0x0FFFFFFF) != unblended_targets.end())
                return true;
        return false;
    };
    // and every pass of the same thing with it: draws of one matrix are one mesh drawn again (a light or shadow modulated over it),
    // and one at the step's place with another blended put the second pass beside the first, darkening the wrong pixels in every
    // in-between frame (measured with the reference renderer's probes at Train Chase's barrier)
    const auto matrix_of = [](const Rec& r) {
        uint64_t h = 0xCBF29CE484222325ull;
        const auto* b = reinterpret_cast<const uint8_t*>(r.vsc.data());
        for (size_t k = 0; k < kMatrixRegisters * sizeof(r.vsc[0]); k++) h = (h ^ b[k]) * 0x100000001B3ull;
        return h;
    };
    std::unordered_map<uint64_t, bool> at_step_matrix;
    if (!unblended_targets.empty())
        for (size_t i = 0; i < count_; i++)
            if (now_[i].kind == Rec::Draw && samples_unblended(*now_[i].s)) at_step_matrix[matrix_of(now_[i])] = true;
    // as the title left them, afterwards
    std::vector<Rec::Header> left;
    std::unordered_map<uint32_t, bool> seen;
    uint32_t header = 0;
    for (size_t i = 0; i < count_; i++)
        for (const Rec::Header& h : now_[i].headers)
            if (!seen[h.va]) {
                seen[h.va] = true;
                Rec::Header l{h.va, {}};
                std::memcpy(l.bytes, kernel::HostPtr(h.va), HeaderSize(h.va));
                left.push_back(l);
            }
    FindStillTargets();
    for (size_t i = 0; i < count_; i++) {
        Rec& r = now_[i];
        if (skip_[i]) continue;
        for (const Rec::Header& h : r.headers) std::memcpy(kernel::HostPtr(h.va), h.bytes, HeaderSize(h.va));
        switch (r.kind) {
            case Rec::Clear:
                UseAsDrawn(r, *r.s);
                inner_->Clear(*r.s, r.a, r.has_bytes ? r.bytes.data() : nullptr, r.b, r.c, r.z, r.d);
                break;
            case Rec::Copy:
                inner_->Copy(r.s1, r.s2, r.a, r.b, r.c);
                break;
            case Rec::Draw: {
                const uint8_t* indices = r.has_bytes ? r.bytes.data() : nullptr;
                State& d = *r.s;
                std::array<State::Stream, 16> streams = d.streams;
                for (const Rec::Kept& kept : r.kept) {
                    const uint32_t h = region_headers_[region_] + header++ * 12;
                    kernel::W32(h, 1);  // a vertex buffer, one reference
                    kernel::W32(h + 4, mem::PhysicalFromVirtual(region_data_[region_] + kept.offset));
                    kernel::W32(h + 8, 0);
                    d.streams[kept.stream].vb = h;
                }
                const bool at_step = !at_step_matrix.empty() && at_step_matrix.count(matrix_of(r));
                if (diag_table)
                    LOGR("gpu: in-between table: %d rec %zu %s vs %08X count %u tex %08X %08X %08X %08X rt %08X", diag_replay, i,
                         alpha == 1 ? "step" : at_step ? "at_step" : r.prev >= 0 ? (r.turning ? "turning" : "blend") : r.derived ? "derived" : r.peer >= 0 ? "peer" : "none",
                         r.s->vs_handle, r.c, r.s->textures[0], r.s->textures[1], r.s->textures[2], r.s->textures[3], r.s->rt.header);
                const bool blend = r.prev >= 0 && alpha != 1 && !at_step;
                Rec::Constants v;  // turned round as kept (Rec::vsc), put back into d.vsc below
                if (blend) {
                    const Rec& p = before_[size_t(r.prev)];
                    if (r.turning) {
                        v = r.vsc;
                        for (uint32_t k = 0; k < kMatrixRegisters; k++) v[k][3] = p.vsc[k][3] + (r.vsc[k][3] - p.vsc[k][3]) * alpha;
                    } else {
                        if (r.lerp_step != steps_seen_) {
                            r.lerp.clear();
                            for (uint32_t k = 0; k < kRegisters; k++) {
                                if (!std::memcmp(p.vsc[k].data(), r.vsc[k].data(), sizeof r.vsc[k])) continue;
                                for (uint32_t c = 0; c < 4; c++)
                                    if (p.vsc[k][c] != r.vsc[k][c] && !Holds(k, p.vsc[k][c], r.vsc[k][c])) r.lerp.push_back(uint16_t(k * 4 + c));
                            }
                            r.lerp_step = steps_seen_;
                            if (ConstLog()) NoteBlended(r, p);
                        }
                        v = r.vsc;
                        for (const uint16_t e : r.lerp) {
                            const uint32_t k = e >> 2, c = e & 3;
                            v[k][c] = p.vsc[k][c] + (r.vsc[k][c] - p.vsc[k][c]) * alpha;
                        }
                    }
                } else if (r.derived && alpha != 1 && !at_step) {
                    v = r.vsc;
                    for (uint32_t k = 0; k < kMatrixRegisters; k++)
                        for (uint32_t c = 0; c < 4; c++) v[k][c] = r.was[k][c] + (r.vsc[k][c] - r.was[k][c]) * alpha;
                } else if (r.peer >= 0 && alpha != 1 && !at_step) {
                    const Rec& q = now_[size_t(r.peer)];
                    const Rec& p = before_[size_t(q.prev)];
                    for (uint32_t k = 0; k < kRegisters; k++) {
                        const bool same = !std::memcmp(r.vsc[k].data(), q.vsc[k].data(), sizeof r.vsc[k]);
                        for (uint32_t c = 0; c < 4; c++)
                            v[k][c] = same ? Between(k, p.vsc[k][c], q.vsc[k][c], alpha) : r.vsc[k][c];
                    }
                } else {
                    v = r.vsc;
                }
                {  // turned back: v[k] is the title's register Transform() + k
                    const uint32_t t = Transform();
                    std::memcpy(&d.vsc[t], &v[0], (kRegisters - t) * sizeof v[0]);
                    if (t) std::memcpy(&d.vsc[0], &v[kRegisters - t], t * sizeof v[0]);
                }
                // a draw without a program takes the fixed function matrices: blended too, or what it draws (a car's light and
                // shadow cast on the road, EoN) stays at the step's place while what is round it is blended, jumping back and forth
                constexpr uint32_t kFixed[] = {2, 3, 16, 17, 18, 19, 256, 257, 258, 259};  // view, projection, texture 0-3, world 0-3
                std::array<std::array<float, 16>, std::size(kFixed)> fixed_was;
                const bool fixed = blend && !(d.vs_handle & 1) && before_[size_t(r.prev)].s;
                if (fixed) {
                    const State& p = *before_[size_t(r.prev)].s;
                    for (size_t i = 0; i < std::size(kFixed); i++) {
                        auto& m = d.transforms[kFixed[i]];
                        fixed_was[i] = m;
                        for (uint32_t e = 0; e < 16; e++) m[e] = p.transforms[kFixed[i]][e] + (m[e] - p.transforms[kFixed[i]][e]) * alpha;
                    }
                }
                UseAsDrawn(r, d);
                if (diag::Perf()) {  // XBE_PERF: a replay's time handing its draws to the renderer, against the rest
                    const auto drawn = Clock::now();
                    inner_->Draw(d, r.a, r.b, r.c, indices);
                    sum_draw_calls_ += std::chrono::duration<double>(Clock::now() - drawn).count();
                } else {
                    inner_->Draw(d, r.a, r.b, r.c, indices);
                }
                if (fixed)
                    for (size_t i = 0; i < std::size(kFixed); i++) d.transforms[kFixed[i]] = fixed_was[i];
                d.streams = streams;
                break;
            }
        }
    }
    for (const Rec::Header& l : left) std::memcpy(kernel::HostPtr(l.va), l.bytes, HeaderSize(l.va));
    region_fence_[region_] = inner_->Fence();
    replay_seconds_ = std::chrono::duration<double>(Clock::now() - began).count();
    sum_replay_ += replay_seconds_, replays_++;
}

// GameInfo::interp_skip_still_targets: a target whose draws a replay would repeat exactly holds the step's picture already, from
// the title's own drawing; its draws and clears are left out of the replays. Exactly: none of its draws blended, given the
// camera's move or following a peer; none sampling a target drawn this step (a pass over the scene changes with it);
// nothing copied into it; nothing reading it between its first and last drawing (memory used twice in a step); its depth
// buffer not another target's; not the picture shown. Juiced's six cube faces and small passes, about a quarter of a step's
// draws, the render thread's time per frame being what holds the frame rate (Harry's XBE_PERF log, 2026-11-15)
void Interpolating::FindStillTargets() {
    if (skip_step_ == steps_seen_ && skip_.size() == count_) return;
    skip_step_ = steps_seen_;
    skip_.assign(count_, 0);
    static const bool on = [] {
        const char* e = std::getenv("XBE_INTERP_SKIP_STILL");
        return e && *e ? std::atoi(e) != 0 : app::kGame.interp_skip_still_targets;
    }();
    if (!on) return;
    constexpr uint32_t kMask = 0x0FFFFFFF;
    struct Target {
        size_t first = SIZE_MAX, last = 0;
        bool ok = true;
    };
    std::unordered_map<uint32_t, Target> targets;
    std::unordered_map<uint32_t, uint32_t> depth_of;  // a depth buffer's target, or 0 when it serves more than one
    const auto target_of = [&](const Rec& r) { return r.s ? r.s->rt.data & kMask : 0u; };
    // a texture's memory as the draw saw it: from its kept header
    const auto texture_data = [](const Rec& r, uint32_t t) -> uint32_t {
        for (const Rec::Header& h : r.headers)
            if (h.va == t) {
                uint32_t v;
                std::memcpy(&v, h.bytes + 4, 4);
                return v & kMask;
            }
        return mem::IsCommitted(t + 7) ? kernel::R32(t + 4) & kMask : 0;
    };
    for (size_t i = 0; i < count_; i++) {
        const Rec& r = now_[i];
        if (r.kind == Rec::Copy) {
            if (const uint32_t d = r.s2.data & kMask) targets[d].ok = false;
            continue;
        }
        const uint32_t d = target_of(r);
        if (!d) continue;
        Target& t = targets[d];
        t.first = std::min(t.first, i), t.last = std::max(t.last, i);
        // blended draws change from one replay to the next, but in a target with no camera move of its own (a cube map's faces
        // following the car, a small pass) that is left at the step's: the car's reflections at the step's rate
        if (r.kind == Rec::Draw && (r.prev >= 0 || r.derived || r.peer >= 0) && camera_targets_.count(d)) t.ok = false;
        if (r.s->zs.header) {
            const uint32_t z = r.s->zs.data & kMask;
            auto it = depth_of.find(z);
            if (it == depth_of.end()) depth_of[z] = d;
            else if (it->second != d) it->second = 0;
        }
    }
    for (size_t i = 0; i < count_; i++) {
        const Rec& r = now_[i];
        const auto read = [&](uint32_t data) {
            auto it = targets.find(data);
            if (it == targets.end()) return;
            if (r.kind == Rec::Draw) {
                const uint32_t d = target_of(r);
                if (auto own = targets.find(d); own != targets.end()) own->second.ok = false;  // samples a target of this step
            }
            if (i > it->second.first && i < it->second.last && target_of(r) != data) it->second.ok = false;
        };
        if (r.kind == Rec::Draw)
            for (uint32_t t : r.s->textures)
                if (t) read(texture_data(r, t));
        if (r.kind == Rec::Copy) read(r.s1.data & kMask);
    }
    // a depth buffer serving several targets: all of them left out or none (the cube's six faces share one)
    for (const auto& [z, d] : depth_of) {
        if (d) continue;
        bool all_ok = true;
        for (size_t i = 0; i < count_ && all_ok; i++)
            if (now_[i].kind != Rec::Copy && now_[i].s->zs.header && (now_[i].s->zs.data & kMask) == z)
                if (auto it = targets.find(target_of(now_[i])); it != targets.end() && !it->second.ok) all_ok = false;
        if (!all_ok)
            for (size_t i = 0; i < count_; i++)
                if (now_[i].kind != Rec::Copy && now_[i].s->zs.header && (now_[i].s->zs.data & kMask) == z)
                    if (auto it = targets.find(target_of(now_[i])); it != targets.end()) it->second.ok = false;
    }
    if (auto it = targets.find(front_data_); it != targets.end()) it->second.ok = false;
    uint32_t skipped = 0;
    for (size_t i = 0; i < count_; i++) {
        const Rec& r = now_[i];
        if (r.kind == Rec::Copy) continue;
        auto it = targets.find(target_of(r));
        if (it != targets.end() && it->second.ok) skip_[i] = 1, skipped++;
    }
    sum_skipped_ += skipped;
    static const bool log = std::getenv("XBE_INTERP_LOG") != nullptr;
    static uint64_t steps = 0, sum = 0, all = 0;
    sum += skipped, all += count_;
    if (log && ++steps % 600 == 0) {
        LOGR("gpu: in-between frames: %llu of %llu records in 600 steps left out of the replays (their targets the step's own)",
             (unsigned long long)sum, (unsigned long long)all);
        sum = all = 0;
    }
}

void Interpolating::Present(const Surface& front) {
    front_data_ = front.data & 0x0FFFFFFF;
    const auto t = Clock::now();
    const double step = std::chrono::duration<double>(t - presented_).count();
    presented_ = t;
    if (kept_full_) LOG_FIRST(4, "gpu: in-between frames: a step of %zu draws has more vertices than are kept, drawn as it is", count_);
    const auto prepare_began = Clock::now();
    const bool blend = On() && count_ && !kept_full_ && Match() && UploadKept();
    sum_prepare_ += std::chrono::duration<double>(Clock::now() - prepare_began).count();
    static const bool test_paced = std::getenv("XBE_INTERP_TEST_PACED") != nullptr;
    if (Test() && blend && test_paced) {
        // as the paced frames go (below): from the step before towards this one, never this step's own picture last
        for (int i = 0; i < Test(); i++) {
            Replay(float(i) / float(Test()));
            inner_->Present(front);
        }
    } else if (Test() && blend) {
        for (int i = 1; i < Test(); i++) {
            Replay(float(i) / float(Test()));
            inner_->Present(front);
        }
        Replay(1);
        inner_->Present(front);
    } else if (!blend || step > kLongestStep || step < kShortestStep) {
        overran_ = false;
        inner_->Present(front);
    } else {
        if (!logged_) LOGR("gpu: in-between frames, a step of %.1f ms at up to %u a second", step * 1000, Rate());
        logged_ = true;
        blended_steps_++;
        // frames for this step, evenly, and all over when the title's timer starts its next: this step's own drawing is taken off,
        // and the replay that ends this. Out of time, the step just drawn is shown as it is
        // a step these frames made late is not the title slowing down (slow motion is): the usual step, unless far past it
        typical_ = typical_ > 0 ? std::min(step, typical_ * 1.02) : step;
        const double usual = step < typical_ * 1.5 ? typical_ : step;
        const double slot = 1.0 / Rate();
        const double drawing = std::chrono::duration<double>(t - first_).count();
        const double span = usual - drawing - kMargin - replay_seconds_;
        // as many as fit, and always one: the picture runs a step behind the title while blending, and the step just drawn shown now
        // and then would jump ahead and back, the whole picture flickering. On a machine that cannot keep that up, none for a while
        const int fit = replay_seconds_ > 0 ? int(std::max(span, 0.0) / replay_seconds_) + 1 : 2;
        // steps coming late again and again are the proof, whatever the cause (a full queue behind vertical sync comes in bursts); one
        // now and then is only the title's own work, an explosion say. Fewer frames, and more after a calm, longer if the last try failed
        // only a step our frames made late counts: one the title was late with on its own (loading, an explosion) says nothing
        // about them. And the limit is kept to the frames a step would get: brought down from kMostFrames by late steps one at a
        // time, each a step it did not hold back, and patience doubled each time, it took minutes to reach 2 at 144 a second and
        // then stayed at 1 for half a minute at a time (Harry, 2026-11-09: 115 then 60 frames a second)
        const int wanted = std::clamp(int(usual / slot + 0.5), 1, kMostFrames);
        const bool late = step > usual * kLate;
        if (late) (overran_ ? late_ours_ : late_title_)++;
        late_ += late && overran_ ? kLateWeight : 0;
        if (late_ > 0) late_--;
        if (late_ >= kLateLimit && limit_ > 1) {
            if (raised_) patience_ = std::min(patience_ * 2, kMostPatience);
            raised_ = false;
            // from the frames a step gets, not from a limit above them (kept there, a short step after a late one held it at 1)
            limit_ = std::max(1, std::min(limit_, wanted) - 1);
            calm_ = 0;
            late_ = 0;
        } else if (++calm_ >= patience_ && limit_ < kMostFrames) {
            limit_++;
            raised_ = true;
            calm_ = 0;
            if (patience_ > kFirstPatience) patience_ = std::max(patience_ * 3 / 4, kFirstPatience);
        }
        const int frames = std::clamp(std::min({int(usual / slot + 0.5), fit, limit_}), 1, kMostFrames);
        sum_drawing_ += drawing, sum_planned_ += frames, sum_limit_ += limit_;
        double spacing = std::min(slot, usual / frames);
        // GameInfo::interp_fill_frames (XBE_INTERP_FILL=0/1 overrides): the frames closer together when at the rate's spacing
        // the last would not fit before the title's next step (Juiced at a cap of 240: 4 planned, the title's own drawing 4 ms
        // of the 16.7, 3 shown, the fps stuck at 180). They are still placed at their own times along the step
        static const bool fill = [] {
            const char* e = std::getenv("XBE_INTERP_FILL");
            const bool on = e && *e ? *e == '1' : app::kGame.interp_fill_frames;
            if (on) LOGR("gpu: in-between frames: a step's frames closer together when they would not all fit");
            return on;
        }();
        if (fill && frames > 1) {
            const double room = span + replay_seconds_ - frame_seconds_ - kOverrun;  // the last frame begun by then
            if (room > 0) spacing = std::min(spacing, std::max(room / (frames - 1), frame_seconds_));
        }
        if (t < rest_until_) {
            overran_ = false;
            inner_->Present(front);
        } else if (replay_seconds_ > usual * kTooLong) {
            rest_until_ = t + std::chrono::seconds(2);
            replay_seconds_ = 0;
            LOGR("gpu: in-between frames: a replay takes too long, none for 2 s");
            overran_ = false;
            inner_->Present(front);
        } else {
            // the title's choice for what the player aims: the step shown at once and carried on from there, up to a step too far when
            // the aim stops, with none of the delay
            const bool ahead = app::kGame.step_ahead && app::kGame.step_ahead();
            // a frame's time includes its present: with vertical sync the render thread waits for the screen, and when its queue is
            // full so does this. One begun that will not be done before the next step would make the title late
            if (ahead) {
                const auto now = Clock::now();
                inner_->Present(front);
                frame_seconds_ += (std::chrono::duration<double>(Clock::now() - now).count() + replay_seconds_ - frame_seconds_) * 0.1;
            }
            overran_ = false;
            for (int i = ahead ? 1 : 0; i < frames; i++) {
                const auto now = Clock::now();
                const double at = std::chrono::duration<double>(now - t).count();
                if ((i || ahead) && at + frame_seconds_ >= span + replay_seconds_) break;
                const double along = std::min(at / usual, 1.0);
                Replay(float(ahead ? 1.0 + along : along));
                inner_->Present(front);
                frame_seconds_ += (std::chrono::duration<double>(Clock::now() - now).count() - frame_seconds_) * 0.1;
                if (i) shown_++;
                // not past the time the title needs for its next step, and the next frame begun early enough to be done by it: the
                // second of two at 144 a second missed by a fraction of a millisecond and was dropped, the step shown alone (Harry's
                // run of a96ead6: 640 to 780 frames for 600 steps)
                const double fits = span + replay_seconds_ - frame_seconds_ - kOverrun;
                const double until = std::max(at, std::min({at + spacing, std::max(span, at), fits}));
                SleepUntil(t + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(until)));
            }
            overran_ = std::chrono::duration<double>(Clock::now() - t).count() > span + replay_seconds_ + kOverrun;
        }
    }
    shown_++;
    steps_++;
    if (t - reported_ > std::chrono::seconds(10)) {
        if (ConstLog() && reported_ != Clock::time_point{}) LogConstStats();
        if (reported_ != Clock::time_point{} && blended_steps_)
        {
            LOGR("gpu: in-between frames: %u frames for %u steps (%u blended) in the last 10 s%s", shown_, steps_, blended_steps_,
                 app::kGame.interp_sampled_targets_at_step ? " (what samples an unblended target at its step's place)" : "");
            const double b = std::max(1u, blended_steps_);
            LOGR("gpu: in-between frames, a step: the title %.2f ms, matching and upload %.2f ms, a replay %.2f ms (%u), frames planned "
                 "%.2f, limit %.2f",
                 sum_drawing_ / b * 1000, sum_prepare_ / std::max(1u, steps_) * 1000, sum_replay_ / std::max(1u, replays_) * 1000,
                 replays_, sum_planned_ / b, sum_limit_ / b);
            LOGR("gpu: in-between frames: late steps: %u after our frames ran over, %u the title's own", late_ours_, late_title_);
            if (diag::Perf())
                LOGR("gpu: in-between frames, a replay: %.2f ms of it handing draws to the renderer", sum_draw_calls_ / std::max(1u, replays_) * 1000);
            if (sum_skipped_)
                LOGR("gpu: in-between frames: %.0f records a step left out of the replays, their targets' pictures the step's own",
                     double(sum_skipped_) / std::max(1u, blended_steps_));
            sum_draw_calls_ = 0;
            sum_skipped_ = 0;
        }
        reported_ = t;
        shown_ = steps_ = blended_steps_ = 0;
        late_ours_ = late_title_ = 0;
        sum_drawing_ = sum_prepare_ = sum_replay_ = sum_planned_ = sum_limit_ = 0;
        replays_ = 0;
    }
    now_.swap(before_);
    steps_seen_++;
    before_count_ = count_;
    count_ = 0;
    kept_bytes_.clear();
    kept_count_ = 0;
    kept_full_ = false;
}
}  // namespace

std::unique_ptr<Backend> CreateInterpolating(std::unique_ptr<Backend> inner) { return std::make_unique<Interpolating>(std::move(inner)); }
}  // namespace gpu
