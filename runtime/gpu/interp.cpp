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
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app/app.h"
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
constexpr float kFar = 0.25f, kLeap = 4.0f;                   // how far a matrix moves in a step and is still the same draw
constexpr uint32_t kFastest = 480;                            // frames a second for an unlimited cap
constexpr uint32_t kHeaderBytes = 20;                         // a resource header: common, data, lock, format, size
constexpr uint32_t kCopyBytes = 64u << 10;                    // a draw's vertices kept when no more than this
constexpr uint32_t kRegionBytes = 8u << 20, kRegionHeaders = 1u << 16;  // kept vertices of a step, in guest memory, two steps' worth
constexpr uint32_t kRegisters = 192, kMatrixRegisters = 4;    // the first are the title's transform in every program seen

struct Rec {
    enum Kind : uint8_t { Clear, Draw, Copy } kind = Draw;
    std::unique_ptr<State> s;  // Clear, Draw; its registers and streams are changed for a replay, and put back
    std::array<std::array<float, 4>, 192> vsc;
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
    float was[4][4];
    int peer = -1;  // none found: a draw of this step sharing its matrix, whose blend it follows register by register
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
        r.vsc = s.vsc;
    }
    // finds each draw's last self; false when there is nothing to blend or the camera was cut
    bool Match();
    void Replay(float alpha);

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
    std::unordered_map<uint64_t, std::vector<int>> found;
    found.reserve(before_count_);
    for (size_t i = before_count_; i-- > 0;)
        if (before_[i].kind == Rec::Draw) found[KeyOf(before_[i])].push_back(int(i));  // popped from the back: in order
    uint32_t draws = 0, matched = 0, moved = 0, jumped = 0;
    for (size_t i = 0; i < count_; i++) {
        Rec& r = now_[i];
        if (r.kind != Rec::Draw) continue;
        draws++;
        auto it = found.find(KeyOf(r));
        if (it == found.end() || it->second.empty()) continue;
        // of those alike (a mesh drawn in many places, a piece of the HUD), the one whose matrix is nearest
        auto& list = it->second;
        size_t best = list.size() - 1;
        float best_distance = 1e30f;
        for (size_t l = list.size(); l-- > 0;) {
            const Rec& c = before_[size_t(list[l])];
            float distance = 0;
            for (uint32_t k = 0; k < kMatrixRegisters; k++)
                for (uint32_t e = 0; e < 4; e++) distance += std::fabs(c.vsc[k][e] - r.vsc[k][e]);
            if (distance < best_distance) best_distance = distance, best = l;
        }
        const int candidate = list[best];
        list.erase(list.begin() + std::ptrdiff_t(best));
        const Rec& was = before_[size_t(candidate)];
        // by the matrix's turn and scale, each row against its own size: its translation crosses zero as an object is passed, a small
        // number with a large change, and the object is the same. Past that only a real leap in it counts
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
        std::vector<Move> moves;
        std::unordered_map<uint32_t, bool> spinning;
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
        const auto camera_of = [&](uint32_t rt) {
            const Move* camera = nullptr;
            for (const Move& o : moves)
                if (o.rt == rt && (!camera || o.votes > camera->votes)) camera = &o;
            return camera;
        };
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
        for (size_t i = 0; i < count_; i++)
            if (now_[i].kind == Rec::Draw && now_[i].prev < 0) fresh[now_[i].s->vs_handle]++;
        for (size_t i = 0; i < count_; i++) {
            Rec& r = now_[i];
            // a few vertices are a sprite, a plate or a car's shadow, on the car and not the street, whatever program draws them
            if (!pieces || r.kind != Rec::Draw || r.prev >= 0 || r.c < kLeastVertices || flat(r.vsc) || spinning.count(r.s->vs_handle)) continue;
            const Move* camera = camera_of(r.s->rt.data);
            if (!camera || camera->votes < kLeastVotes) continue;
            const auto seen = program.find(r.s->vs_handle);
            if (seen == program.end() || seen->second[1] * 2 <= seen->second[0] || seen->second[1] < fresh[r.s->vs_handle] * kFreshShare) continue;
            const Rec* near = nullptr;
            float nearest = 1e30f;
            for (size_t j = 0; j < count_; j++) {
                const Rec& o = now_[j];
                if (o.kind != Rec::Draw || o.prev < 0 || o.s->vs_handle != r.s->vs_handle || o.s->rt.data != r.s->rt.data) continue;
                float distance = 0;
                for (uint32_t k = 0; k < kMatrixRegisters; k++)
                    for (uint32_t c = 0; c < 4; c++) distance += std::fabs(o.vsc[k][c] - r.vsc[k][c]);
                if (distance < nearest) nearest = distance, near = &o;
            }
            double d[4][4];
            if (!near || !move_of(*near, d) || !same_move(d, camera->d)) continue;
            for (int a = 0; a < 4; a++)
                for (int b = 0; b < 4; b++) {
                    double t = 0;
                    for (int c = 0; c < 4; c++) t += camera->d[a][c] * r.vsc[size_t(c)][size_t(b)];
                    r.was[a][b] = float(t);
                }
            r.derived = true;
        }
    }
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
    for (size_t i = 0; i < count_; i++) {
        Rec& r = now_[i];
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
                if (blend) {
                    const Rec& p = before_[size_t(r.prev)];
                    if (r.turning) {
                        d.vsc = r.vsc;
                        for (uint32_t k = 0; k < kMatrixRegisters; k++) d.vsc[k][3] = p.vsc[k][3] + (r.vsc[k][3] - p.vsc[k][3]) * alpha;
                    } else {
                        for (uint32_t k = 0; k < kRegisters; k++)
                            for (uint32_t c = 0; c < 4; c++) d.vsc[k][c] = p.vsc[k][c] + (r.vsc[k][c] - p.vsc[k][c]) * alpha;
                    }
                } else if (r.derived && alpha != 1 && !at_step) {
                    d.vsc = r.vsc;
                    for (uint32_t k = 0; k < kMatrixRegisters; k++)
                        for (uint32_t c = 0; c < 4; c++) d.vsc[k][c] = r.was[k][c] + (r.vsc[k][c] - r.was[k][c]) * alpha;
                } else if (r.peer >= 0 && alpha != 1 && !at_step) {
                    const Rec& q = now_[size_t(r.peer)];
                    const Rec& p = before_[size_t(q.prev)];
                    for (uint32_t k = 0; k < kRegisters; k++) {
                        const bool same = !std::memcmp(r.vsc[k].data(), q.vsc[k].data(), sizeof r.vsc[k]);
                        for (uint32_t c = 0; c < 4; c++)
                            d.vsc[k][c] = same ? p.vsc[k][c] + (q.vsc[k][c] - p.vsc[k][c]) * alpha : r.vsc[k][c];
                    }
                } else {
                    d.vsc = r.vsc;
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
                inner_->Draw(d, r.a, r.b, r.c, indices);
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

void Interpolating::Present(const Surface& front) {
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
        const double spacing = std::min(slot, usual / frames);
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
                std::this_thread::sleep_until(t + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(until)));
            }
            overran_ = std::chrono::duration<double>(Clock::now() - t).count() > span + replay_seconds_ + kOverrun;
        }
    }
    shown_++;
    steps_++;
    if (t - reported_ > std::chrono::seconds(10)) {
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
        }
        reported_ = t;
        shown_ = steps_ = blended_steps_ = 0;
        late_ours_ = late_title_ = 0;
        sum_drawing_ = sum_prepare_ = sum_replay_ = sum_planned_ = sum_limit_ = 0;
        replays_ = 0;
    }
    now_.swap(before_);
    before_count_ = count_;
    count_ = 0;
    kept_bytes_.clear();
    kept_count_ = 0;
    kept_full_ = false;
}
}  // namespace

std::unique_ptr<Backend> CreateInterpolating(std::unique_ptr<Backend> inner) { return std::make_unique<Interpolating>(std::move(inner)); }
}  // namespace gpu
