// In-between frames for a title that draws one frame a step of its game (GameInfo::step_interpolation). Each step's clears, draws
// and copies are kept as they pass; when it presents, the step is drawn again a few times with the vertex shader constants blended
// from the step before, by the time since it presented, at the rate of the frame cap, until the title has to go on. A
// draw of the step before is found by what it draws, in order. Its memory is the title's until its next step, and the title is
// waiting here. So the picture is up to one step late; nothing is drawn beyond what the title gave. No blend after a camera cut.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
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

bool RopeCheck();  // XBE_INTERP_ROPE_CHECK, below

// GameInfo::interp_near_margin: a named draw this close to the camera (its w, the view depth) counts as at the near
// plane; XBE_INTERP_NEAR_W overrides. The nearest named vertex of each step is kept for the log
float NearMargin() {
    static const float m = [] {
        const char* e = std::getenv("XBE_INTERP_NEAR_W");
        const float v = e && *e ? float(std::atof(e)) : app::kGame.interp_near_margin;
        if (v > 0) LOGR("gpu: in-between frames: a named draw within %.1f of the camera holds its step", v);
        return v;
    }();
    return m;
}
float g_nearest_w = 1e30f;

// GameInfo::interp_near_hold names a draw by its vertex program's code (its handle is an address the title allocates,
// which moves between runs: Fight Night Round 2's rope program was B0003131 in one, B0103131 in another)
uint64_t ProgramHash(const State& s) {
    if (!(s.vs_handle & 1) || !s.vs || s.vs->program.empty()) return 0;
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
    return s.vs->hash;
}
bool NearHoldNames(const State& s, uint32_t count) { return app::kGame.interp_near_hold(ProgramHash(s), s.textures[0], count); }
// GameInfo::interp_named_transform: the first of the four registers a named draw's program keeps its transform in;
// -1 when the program only transforms v0 by four constants (gpu::MatrixProgram), else the first four
int NamedTransform(const State& s) {
    const int k = app::kGame.interp_named_transform ? app::kGame.interp_named_transform(ProgramHash(s)) : -1;
    return k >= 0 && k + 3 < 192 ? k : -1;
}
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
    bool near_clipped = false;  // GameInfo::interp_near_hold: a vertex at or behind the near plane this step
    bool named = false;         // GameInfo::interp_near_hold names it: not matched, moved by the camera's move alone
    int camera_moved = -1;      // named: 1 when the camera's move was given to it
    uint32_t xf = 0;            // the first of the registers holding its transform (GameInfo::interp_named_transform)
    bool xf_known = false;      // given by the title, or the program only transforms v0 by those four (gpu::MatrixProgram)
    bool offscreen = false;     // named: drawn into a target that is not a frame buffer
    float box[6] = {};          // named: v0's bounds (min x y z, max x y z), for the camera's earlier place
    bool has_box = false;
    float was[4][4];
    int peer = -1;  // none found: a draw of this step sharing its matrix, whose blend it follows register by register
};

// the surfaces presented lately: the frame buffers. A title rotating two or three of them draws each step into another
// (Fight Night Round 2: 00A73000, 00BDC000, 0090A000), so in a draw's key they are one target, the frame
std::array<std::atomic<uint32_t>, 4> g_presented{};
bool Presented(uint32_t data) {
    for (const auto& p : g_presented)
        if (data && p.load(std::memory_order_relaxed) == data) return true;
    return false;
}
void NotePresented(uint32_t data) {
    if (!data || Presented(data)) return;
    static std::atomic<uint32_t> next{0};
    g_presented[next++ % g_presented.size()].store(data, std::memory_order_relaxed);
}

uint64_t KeyOf(const Rec& r) {
    const State& s = *r.s;
    uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001B3ull; };
    mix(r.a), mix(r.b), mix(r.c), mix(s.vs_handle), mix(s.rt.header);
    // and the target's memory: a title draws one surface header into two pictures on alternate steps (EoN's driving: its
    // environment maps at 811C0000 and 813C0000 through one header), and a draw matched to the other picture's of the step
    // before blended two views into one. Unmatched, they are drawn at the step's place, and what samples them with them
    // (GameInfo::interp_sampled_targets_at_step)
    mix(Presented(s.rt.data) ? 1 : s.rt.data);
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
        if (count_ == 0) CheckRopeMemory();
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
        if (count_ == 0) CheckRopeMemory();
        Rec& r = Next(Rec::Draw);
        Keep(r, s);
        r.a = prim, r.b = start, r.c = count;
        AsDrawn(r, s);
        r.headers.clear();
        for (const auto& stream : s.streams) AddHeader(r, stream.vb);
        for (uint32_t t : s.textures) AddHeader(r, t);
        KeepVertices(r, s, start, count, indices);
        if (app::kGame.interp_near_hold && NearHoldNames(s, count)) {
            r.named = true;
            r.offscreen = !Presented(s.rt.data);
            const int xf = NamedTransform(s);
            r.xf = xf >= 0 ? uint32_t(xf) : 0;
            r.near_clipped = !r.offscreen && NearClipped(s, start, count, indices, r.box, r.has_box, xf);
            if (s.vs && s.vs->matrix == -2) s.vs->matrix = MatrixProgram(s.vs->program);
            r.xf_known = xf >= 0 || (s.vs && s.vs->matrix == 0);
            if (RopeCheck())
                for (const Rec::Kept& kp : r.kept) {
                    const uint32_t va = mem::VirtualFromPhysical(kernel::R32(s.streams[kp.stream].vb + 4));
                    rope_now_.push_back({va, std::vector<uint8_t>(kept_bytes_.begin() + kp.offset, kept_bytes_.begin() + kp.offset + kp.bytes)});
                }
        }
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
    // a frame cap above the console's own rate (GameInfo::vblank_hz: 60, or a PAL title's 50, whose 60 then has room)
    static bool On() {
        return app::kGame.step_interpolation && app::kGame.step_interpolation() && (Test() || Rate() > app::g_pace_hz.load());
    }
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
        if (!va) return;
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
        r.near_clipped = false;
        r.named = false;
        r.camera_moved = -1;
        r.xf = 0;
        r.xf_known = false;
        r.offscreen = false;
        r.has_box = false;
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
    // a vertex of the draw at or behind the near plane (clip z < 0 or w <= 0) through the program's position matrix (one
    // that only transforms v0 by four constants: gpu::MatrixProgram); v0 as FLOAT3, FLOAT4 or SHORT3 (D3DVSDT 0x32, 0x42,
    // 0x35) in its stream. False when it cannot be read
    static bool NearClipped(const State& s, uint32_t start, uint32_t count, const uint8_t* indices, float box[6], bool& has_box,
                            int given = -1) {
        if (!(s.vs_handle & 1) || !s.vs) return false;
        if (s.vs->matrix == -2) s.vs->matrix = MatrixProgram(s.vs->program);
        const int k = given >= 0 ? given : s.vs->matrix;
        const auto& in = s.vs->inputs[0];
        if (k < 0 || in.stream < 0 || (in.type != 0x32 && in.type != 0x42 && in.type != 0x35)) return false;
        const auto& st = s.streams[size_t(in.stream)];
        if (!st.vb || !st.stride) return false;
        const uint32_t data = kernel::R32(st.vb + 4);
        if (!data) return false;
        const uint32_t va = mem::VirtualFromPhysical(data);
        uint32_t most = start + count;
        if (indices) {
            most = 0;
            for (uint32_t i = 0; i < count; i++) {
                uint16_t v;
                std::memcpy(&v, indices + size_t(i) * 2, 2);
                most = std::max(most, uint32_t(v) + 1);
            }
            most += s.base_vertex;
        }
        if (!most || !mem::IsCommitted(va) || !mem::IsCommitted(va + most * st.stride - 1)) return false;
        const auto* base = static_cast<const uint8_t*>(kernel::HostPtr(va));
        const auto& z = s.vsc[size_t(k) + 2];
        const auto& w = s.vsc[size_t(k) + 3];
        bool clipped = false;
        box[0] = box[1] = box[2] = 1e30f;
        box[3] = box[4] = box[5] = -1e30f;
        for (uint32_t v = 0; v < most; v++) {
            const uint8_t* at = base + size_t(v) * st.stride + in.offset;
            float p[3];
            if (in.type == 0x35) {
                int16_t q[3];
                std::memcpy(q, at, 6);
                p[0] = q[0], p[1] = q[1], p[2] = q[2];
            } else {
                std::memcpy(p, at, 12);
            }
            for (int c = 0; c < 3; c++) box[c] = std::min(box[c], p[c]), box[3 + c] = std::max(box[3 + c], p[c]);
            const float vw = w[0] * p[0] + w[1] * p[1] + w[2] * p[2] + w[3];
            g_nearest_w = std::min(g_nearest_w, vw);
            if (vw <= NearMargin() || z[0] * p[0] + z[1] * p[1] + z[2] * p[2] + z[3] < 0) clipped = true;
        }
        has_box = true;
        return clipped;
    }
    // GameInfo::interp_near_jump: a box corner (in front in both) whose place on screen moves more than this (of the
    // screen's half width) from the earlier matrix to this step's: the camera's move given it is not the camera's
    static bool BoxJumped(const float box[6], const float was[4][4], const std::array<std::array<float, 4>, 192>& vsc, uint32_t xf) {
        static const float most = [] {
            const char* e = std::getenv("XBE_INTERP_NEAR_JUMP");
            const float v = e && *e ? float(std::atof(e)) : app::kGame.interp_near_jump;
            if (v > 0) LOGR("gpu: in-between frames: a named draw moving more than %.2f of the screen in a step holds it", v);
            return v;
        }();
        if (most <= 0) return false;
        for (int i = 0; i < 8; i++) {
            const float p[4] = {box[(i & 1) ? 3 : 0], box[(i & 2) ? 4 : 1], box[(i & 4) ? 5 : 2], 1};
            float a[4], b[4];
            for (int r = 0; r < 4; r++) {
                a[r] = was[r][0] * p[0] + was[r][1] * p[1] + was[r][2] * p[2] + was[r][3];
                const auto& n = vsc[xf + r];
                b[r] = n[0] * p[0] + n[1] * p[1] + n[2] * p[2] + n[3];
            }
            if (a[3] <= 0 || b[3] <= 0) continue;
            if (std::fabs(a[0] / a[3] - b[0] / b[3]) > most || std::fabs(a[1] / a[3] - b[1] / b[3]) > most) return true;
        }
        return false;
    }
    // GameInfo::interp_near_hold's draw, given the camera's move: a kept vertex clipped by the near plane (w at most the
    // margin, or z below 0) through one of the step before's matrix (was) and this step's, not both. False where its
    // transform is not known (as the near plane test always was there: Fight Night Round 2's 653-index ropes, whose
    // program does more than transform v0, were never held by it and showed whole), true where it cannot be read
    bool Crosses(const Rec& r) const {
        if (!r.xf_known) return false;
        const State& s = *r.s;
        if (!(s.vs_handle & 1) || !s.vs) return true;
        const auto& in = s.vs->inputs[0];
        if (in.stream < 0 || (in.type != 0x32 && in.type != 0x42 && in.type != 0x35)) return true;
        const uint32_t stride = s.streams[size_t(in.stream)].stride;
        const Rec::Kept* kept = nullptr;
        for (const Rec::Kept& k : r.kept)
            if (k.stream == uint32_t(in.stream)) kept = &k;
        if (!kept || !stride) return true;
        const float m = NearMargin();
        const auto& now = r.vsc;
        for (uint32_t v = 0; v + 1 <= kept->bytes / stride; v++) {
            const uint8_t* at = kept_bytes_.data() + kept->offset + size_t(v) * stride + in.offset;
            if (size_t(at - kept_bytes_.data()) + 12 > kept_bytes_.size()) break;
            float p[3];
            if (in.type == 0x35) {
                int16_t q[3];
                std::memcpy(q, at, 6);
                p[0] = q[0], p[1] = q[1], p[2] = q[2];
            } else {
                std::memcpy(p, at, 12);
            }
            const auto& z1 = now[r.xf + 2];
            const auto& w1 = now[r.xf + 3];
            const bool a = r.was[3][0] * p[0] + r.was[3][1] * p[1] + r.was[3][2] * p[2] + r.was[3][3] <= m ||
                           r.was[2][0] * p[0] + r.was[2][1] * p[1] + r.was[2][2] * p[2] + r.was[2][3] < 0;
            const bool b = w1[0] * p[0] + w1[1] * p[1] + w1[2] * p[2] + w1[3] <= m || z1[0] * p[0] + z1[1] * p[1] + z1[2] * p[2] + z1[3] < 0;
            if (a != b) return true;
        }
        return false;
    }
    // a box's corners through a matrix's z and w rows (rows 2 and 3): any at or behind the near plane
    static bool BoxClipped(const float box[6], const float m[4][4]) {
        for (int i = 0; i < 8; i++) {
            const float p[3] = {box[(i & 1) ? 3 : 0], box[(i & 2) ? 4 : 1], box[(i & 4) ? 5 : 2]};
            const float z = m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3];
            const float w = m[3][0] * p[0] + m[3][1] * p[1] + m[3][2] * p[2] + m[3][3];
            if (w <= NearMargin() || z < 0) return true;
        }
        return false;
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
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> rope_now_, rope_before_;  // XBE_INTERP_ROPE_CHECK
    float marked_alpha_ = -1;  // XBE_INTERP_MARK: the replay just drawn, -1 the title's own picture
    void Show(const Surface& front);
    void CheckRopeMemory();
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

// XBE_INTERP_ROPE_CHECK=1 (a test): the named draws' vertex memory, as kept at their draws, compared at the next
// step's first draw: rewritten by then, a renderer reading it late (the render thread a step behind) draws torn
bool RopeCheck() {
    static const bool on = [] {
        const char* e = std::getenv("XBE_INTERP_ROPE_CHECK");
        return e && *e == '1';
    }();
    return on;
}

void Interpolating::CheckRopeMemory() {
    if (!RopeCheck()) return;
    uint32_t changed = 0, total = 0;
    for (const auto& [va, bytes] : rope_before_) {
        total++;
        if (mem::IsCommitted(va) && std::memcmp(kernel::HostPtr(va), bytes.data(), bytes.size())) changed++;
    }
    static uint32_t steps = 0, changed_all = 0, total_all = 0;
    steps++, changed_all += changed, total_all += total;
    if (steps % 100 == 0) {
        LOGR("gpu: in-between frames: rope check, %u of %u named streams rewritten by the next step's first draw (last 100 steps)",
             changed_all, total_all);
        changed_all = total_all = 0;
    }
    rope_before_.clear();
}

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
        if (!st.vb || !st.stride) continue;
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
// Skinned meshes keep a rigid matrix per bone: four registers from a multiple of four, a rotation in the first three
// rows with the position in their fourth column, and (0 0 0 1) last. Blended a number at a time, a bone that turns far
// in a step (Fight Night Round 2: a boxer's torso snapping round as a punch lands) shrinks and shears in between: the
// body crumpled for a frame and flickered. Such a group, rigid in both steps, is blended as a rotation (quaternions,
// the shorter way round, normalised) with its position straight between; anything else stays blended as numbers
namespace {
using Regs = std::array<std::array<float, 4>, 192>;
bool Rigid(const Regs& m, uint32_t k) {
    const auto& w = m[k + 3];
    if (w[0] != 0 || w[1] != 0 || w[2] != 0 || w[3] != 1) return false;
    for (int i = 0; i < 3; i++)
        for (int j = i; j < 3; j++) {
            float d = 0;
            for (int c = 0; c < 3; c++) d += m[k + i][c] * m[k + j][c];
            if (std::fabs(d - (i == j ? 1.0f : 0.0f)) > 0.02f) return false;
        }
    return true;
}
// rows of a rotation to (x y z w)
void ToQuat(const Regs& m, uint32_t k, double q[4]) {
    const double a = m[k][0], b = m[k][1], c = m[k][2], d = m[k + 1][0], e = m[k + 1][1], f = m[k + 1][2], g = m[k + 2][0],
                 h = m[k + 2][1], i = m[k + 2][2];
    const double t = a + e + i;
    if (t > 0) {
        const double s = std::sqrt(t + 1) * 2;
        q[3] = s / 4, q[0] = (h - f) / s, q[1] = (c - g) / s, q[2] = (d - b) / s;
    } else if (a > e && a > i) {
        const double s = std::sqrt(1 + a - e - i) * 2;
        q[3] = (h - f) / s, q[0] = s / 4, q[1] = (b + d) / s, q[2] = (c + g) / s;
    } else if (e > i) {
        const double s = std::sqrt(1 + e - a - i) * 2;
        q[3] = (c - g) / s, q[0] = (b + d) / s, q[1] = s / 4, q[2] = (f + h) / s;
    } else {
        const double s = std::sqrt(1 + i - a - e) * 2;
        q[3] = (d - b) / s, q[0] = (c + g) / s, q[1] = (f + h) / s, q[2] = s / 4;
    }
}
}  // namespace

// the largest turn of a rigid group between two steps, degrees (0 when none is rigid in both)
float MostTurn(const Regs& from, const Regs& to) {
    float most = 0;
    for (uint32_t k = 0; k + 3 < 192; k += 4) {
        if (!Rigid(from, k) || !Rigid(to, k)) continue;
        double a[4], b[4];
        ToQuat(from, k, a);
        ToQuat(to, k, b);
        const double dot = std::min(1.0, std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]));
        most = std::max(most, float(2 * std::acos(dot) * 180 / 3.14159265358979));
    }
    return most;
}

void BlendRigid(const Regs& from, const Regs& to, float alpha, Regs& out) {
    for (uint32_t k = 0; k + 3 < 192; k += 4) {
        if (!Rigid(from, k) || !Rigid(to, k)) continue;
        double a[4], b[4], q[4];
        ToQuat(from, k, a);
        ToQuat(to, k, b);
        const double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        const double sign = dot < 0 ? -1 : 1;
        double n = 0;
        for (int c = 0; c < 4; c++) q[c] = a[c] + (sign * b[c] - a[c]) * alpha, n += q[c] * q[c];
        if (n < 1e-12) continue;
        n = 1 / std::sqrt(n);
        const double x = q[0] * n, y = q[1] * n, z = q[2] * n, w = q[3] * n;
        const double r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                                {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                                {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
        for (int i = 0; i < 3; i++)
            for (int c = 0; c < 3; c++) out[k + i][c] = float(r[i][c]);  // the fourth column, the position, as blended
    }
}

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
        if (r.named) continue;  // GameInfo::interp_near_hold: the camera's move alone, below
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
        // a rigid first matrix (a skinned mesh's first bone) turned far is blended as a rotation (BlendRigid): only a
        // matrix that is not, with its view and projection in it, stays at its step's turn. Fight Night Round 2's
        // boxers: some pieces of a body taken as turning and left at the step while the rest blended, torn apart
        if (turned && Rigid(was.vsc, 0) && Rigid(r.vsc, 0)) turned = false;
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
        const bool on = e ? std::atoi(e) != 0 : app::kGame.interp_pieces;
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
        // GameInfo::interp_near_hold's draws (a ring's rope sections, told apart by their matrices alone, as many as are in
        // view): not matched, which paired sections with their neighbours and broke a rope into steps; each takes the
        // camera's move (step before = move * now), so a rope moves as one with the scene around it
        for (size_t i = 0; i < count_; i++) {
            Rec& r = now_[i];
            if (r.kind != Rec::Draw || !r.named || r.offscreen) continue;
            const auto& w = r.vsc[r.xf + 3];  // a screen's 2D has no perspective row
            if (std::fabs(w[0]) + std::fabs(w[1]) + std::fabs(w[2]) < 1e-6f) continue;
            const Move* camera = camera_of(r.s->rt.data);
            if (!camera || camera->votes < kLeastVotes) continue;
            for (int a = 0; a < 4; a++)
                for (int b = 0; b < 4; b++) {
                    double t = 0;
                    for (int c = 0; c < 4; c++) t += camera->d[a][c] * r.vsc[r.xf + size_t(c)][size_t(b)];
                    r.was[a][b] = float(t);
                }
            r.derived = true;
            r.camera_moved = 1;
        }
    }
    LogJumps();
    // GameInfo::interp_near_hold: a draw it names that reaches the near plane in this step or its match in the step before
    // (a ring rope the camera passes by) has no in-between picture that is right: blended, its vertices through w 0 are
    // thrown across the screen; its parts behind the camera are cut where the game never drew them. The step is shown as
    // the title drew it, with no frames in between
    for (size_t i = 0; app::kGame.interp_near_hold && i < count_; i++) {
        const Rec& r = now_[i];
        if (r.kind != Rec::Draw) continue;
        if (!NearHoldNames(*r.s, r.c)) continue;
        // given the camera's move: one of its vertices on one side of the near plane in this step and on the other in the
        // step before (one behind the camera in both stays behind it in between, the blend being linear, and is clipped as
        // the title's own is: a ring's ropes always run behind the camera somewhere); without it, at the near plane now
        const char* why = r.camera_moved > 0 ? (Crosses(r) ? "crossing the near plane"
                                                 : r.has_box && BoxJumped(r.box, r.was, r.vsc, r.xf) ? "moved too far by the camera's move"
                                                                                                     : nullptr)
                          : r.near_clipped               ? "at the near plane"
                          : !r.offscreen                 ? "without the camera's move"
                                                         : nullptr;
        if (why) {
            static uint32_t held = 0;
            if (held++ % 50 == 0) LOGR("gpu: in-between frames: a step held, a named draw %s (%u so far)", why, held);
            return false;
        }
    }
    // GameInfo::interp_bone_hold (XBE_INTERP_BONE_HOLD overrides): a step in which a matched draw's rigid group (a
    // bone) turns further is shown as drawn; the turns of each step are counted for the log either way
    {
        static const float hold = [] {
            const char* e = std::getenv("XBE_INTERP_BONE_HOLD");
            const float v = e && *e ? float(std::atof(e)) : app::kGame.interp_bone_hold;
            if (v > 0) LOGR("gpu: in-between frames: a bone turning more than %.0f degrees in a step holds it", v);
            return v;
        }();
        float most = 0;
        for (size_t i = 0; i < count_; i++)
            if (now_[i].kind == Rec::Draw && now_[i].prev >= 0) most = std::max(most, MostTurn(before_[size_t(now_[i].prev)].vsc, now_[i].vsc));
        static uint32_t buckets[6] = {}, steps = 0;
        constexpr float kEdges[5] = {10, 30, 60, 90, 135};
        int b = 0;
        while (b < 5 && most >= kEdges[b]) b++;
        buckets[b]++;
        if (++steps % 500 == 0) {
            LOGR("gpu: in-between frames: most bone turn a step: <10 %u, 10-30 %u, 30-60 %u, 60-90 %u, 90-135 %u, 135+ %u",
                 buckets[0], buckets[1], buckets[2], buckets[3], buckets[4], buckets[5]);
            std::fill(std::begin(buckets), std::end(buckets), 0u);
        }
        if (hold > 0 && most > hold) return false;
    }
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
                        BlendRigid(p.vsc, r.vsc, alpha, d.vsc);
                    }
                } else if (r.derived && alpha != 1 && !at_step) {
                    d.vsc = r.vsc;
                    for (uint32_t k = 0; k < kMatrixRegisters; k++)
                        for (uint32_t c = 0; c < 4; c++)
                            d.vsc[r.xf + k][c] = r.was[k][c] + (r.vsc[r.xf + k][c] - r.was[k][c]) * alpha;
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
    marked_alpha_ = alpha;
}

// XBE_INTERP_MARK=1 (a test): a 24 pixel square at the frame's top left says what is shown, red the title's own
// picture, green a frame in between (brighter further from the step before), for desktop grabs to be told apart
void Interpolating::Show(const Surface& front) {
    static const bool mark = [] {
        const char* e = std::getenv("XBE_INTERP_MARK");
        return e && *e == '1';
    }();
    if (mark && count_) {
        const State& s = *now_[count_ - 1].s;
        if (s.rt.data == front.data) {
            const int32_t rect[4] = {0, 0, 24, 24};
            const uint32_t color = marked_alpha_ < 0 ? 0xFFFF0000u : 0xFF000000u | (uint32_t(64 + marked_alpha_ * 191) << 8);
            inner_->Clear(s, 1, reinterpret_cast<const uint8_t*>(rect), 0xF0, color, 1, 0);  // D3DCLEAR_TARGET (0xF0 on the console)
        }
    }
    marked_alpha_ = -1;
    inner_->Present(front);
}

void Interpolating::Present(const Surface& front) {
    NotePresented(front.data);
    // the step's own draws, up to here: the renderer reads their vertices when it comes to them, and the title writes some
    // in place for its next step without locking them (Fight Night Round 2's ring ropes: one stream in five of them
    // rewritten before its next step's first draw, XBE_INTERP_ROPE_CHECK). Behind by the frames in between, the renderer
    // drew ropes torn into the next step's, flickering; so the title goes on only once they have been read (below)
    const uint32_t own = On() ? inner_->Fence() : 0;
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
            Show(front);
        }
    } else if (Test() && blend) {
        for (int i = 1; i < Test(); i++) {
            Replay(float(i) / float(Test()));
            Show(front);
        }
        Replay(1);
        Show(front);
    } else if (!blend || step > kLongestStep || step < kShortestStep) {
        overran_ = false;
        Show(front);
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
            Show(front);
        } else if (replay_seconds_ > usual * kTooLong) {
            rest_until_ = t + std::chrono::seconds(2);
            replay_seconds_ = 0;
            LOGR("gpu: in-between frames: a replay takes too long, none for 2 s");
            overran_ = false;
            Show(front);
        } else {
            // the title's choice for what the player aims: the step shown at once and carried on from there, up to a step too far when
            // the aim stops, with none of the delay
            const bool ahead = app::kGame.step_ahead && app::kGame.step_ahead();
            // a frame's time includes its present: with vertical sync the render thread waits for the screen, and when its queue is
            // full so does this. One begun that will not be done before the next step would make the title late
            if (ahead) {
                const auto now = Clock::now();
                Show(front);
                frame_seconds_ += (std::chrono::duration<double>(Clock::now() - now).count() + replay_seconds_ - frame_seconds_) * 0.1;
            }
            overran_ = false;
            for (int i = ahead ? 1 : 0; i < frames; i++) {
                const auto now = Clock::now();
                const double at = std::chrono::duration<double>(now - t).count();
                if ((i || ahead) && at + frame_seconds_ >= span + replay_seconds_) break;
                const double along = std::min(at / usual, 1.0);
                Replay(float(ahead ? 1.0 + along : along));
                Show(front);
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
    if (own) inner_->WaitFence(own);
    {
        // the nearest named vertex's view depth each step, over 10 s (for GameInfo::interp_near_margin)
        static uint32_t buckets[8] = {}, steps = 0;
        static Clock::time_point since = Clock::now();
        constexpr float kEdges[7] = {0, 25, 50, 100, 200, 400, 800};
        if (g_nearest_w < 1e29f) {
            int b = 0;
            while (b < 7 && g_nearest_w >= kEdges[b]) b++;
            buckets[b]++, steps++;
        }
        g_nearest_w = 1e30f;
        if (Clock::now() - since > std::chrono::seconds(10) && steps) {
            LOGR("gpu: in-between frames: nearest named w per step: <0 %u, 0-25 %u, 25-50 %u, 50-100 %u, 100-200 %u, 200-400 %u, "
                 "400-800 %u, 800+ %u",
                 buckets[0], buckets[1], buckets[2], buckets[3], buckets[4], buckets[5], buckets[6], buckets[7]);
            std::fill(std::begin(buckets), std::end(buckets), 0u);
            steps = 0;
            since = Clock::now();
        }
    }
    now_.swap(before_);
    before_count_ = count_;
    count_ = 0;
    kept_bytes_.clear();
    kept_count_ = 0;
    kept_full_ = false;
    rope_before_.swap(rope_now_);
    rope_now_.clear();
}
}  // namespace

std::unique_ptr<Backend> CreateInterpolating(std::unique_ptr<Backend> inner) { return std::make_unique<Interpolating>(std::move(inner)); }
}  // namespace gpu
