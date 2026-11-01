// In-between frames for a title that draws one frame a step of its game (GameInfo::step_interpolation). Each step's clears, draws
// and copies are kept as they pass; when it presents, the step is drawn again a few times with the vertex shader constants blended
// from the step before, by the time since it presented, at the rate of the frame cap, until the title has to go on. A
// draw of the step before is found by what it draws, in order. Its memory is the title's until its next step, and the title is
// waiting here. So the picture is up to one step late; nothing is drawn beyond what the title gave. No blend after a camera cut.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace gpu {
namespace {
using Clock = std::chrono::steady_clock;
constexpr double kLongestStep = 0.15, kShortestStep = 0.005;  // seconds; past these the steps are not moving a picture along
constexpr double kMargin = 0.002;                             // seconds kept for the title's own work before its first draw
constexpr int kMostFrames = 32;
constexpr float kFar = 0.25f, kFarLeast = 0.5f;               // how far one register of a matrix moves in a step and is still the same draw
constexpr uint32_t kFastest = 480;                            // frames a second for an unlimited cap
constexpr uint32_t kHeaderBytes = 20;                         // a resource header: common, data, lock, format, size
constexpr uint32_t kCopyBytes = 64u << 10;                    // a draw's vertices kept when no more than this
constexpr uint32_t kRegionBytes = 8u << 20, kRegionHeaders = 4096;  // kept vertices of a step, in guest memory, two steps' worth
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
    int prev = -1;  // the same draw in the step before
};

uint64_t KeyOf(const Rec& r) {
    const State& s = *r.s;
    uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001B3ull; };
    mix(r.a), mix(r.b), mix(r.c), mix(s.vs_handle), mix(s.rt.header);
    for (uint32_t t : s.textures) mix(t);
    mix(s.streams[0].vb), mix(s.index_data);
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
        r.has_bytes = rects != nullptr;
        if (rects) r.bytes.assign(rects, rects + size_t(count) * 16);
    }
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        inner_->Draw(s, prim, start, count, indices);
        if (!On()) return;
        Rec& r = Next(Rec::Draw);
        Keep(r, s);
        r.a = prim, r.b = start, r.c = count;
        r.headers.clear();
        const auto add = [&](uint32_t va) {
            if (!va) return;
            Rec::Header h{va, {}};
            std::memcpy(h.bytes, kernel::HostPtr(va), HeaderSize(va));
            r.headers.push_back(h);
        };
        for (const auto& stream : s.streams) add(stream.vb);
        for (uint32_t t : s.textures) add(t);
        KeepVertices(r, s, start, count, indices);
        for (uint32_t p : s.palettes) add(p);
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
    void KeepVertices(Rec& r, const State& s, uint32_t start, uint32_t count, const uint8_t* indices);
    void UploadKept();
    Rec& Next(Rec::Kind kind) {
        if (!count_) first_ = Clock::now();
        if (count_ == now_.size()) now_.emplace_back();
        Rec& r = now_[count_++];
        r.kind = kind;
        r.prev = -1;
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
    uint32_t region_ = 0, region_data_[2] = {}, region_headers_[2] = {}, region_fence_[2] = {};
    Clock::time_point presented_{}, first_{};
    double replay_seconds_ = 0, typical_ = 0;
    bool logged_ = false;
    Clock::time_point reported_{};
    uint32_t shown_ = 0, steps_ = 0, blended_steps_ = 0;
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
        if (!st.vb || !st.stride) continue;
        const uint64_t bytes = uint64_t(vertices) * st.stride;
        if (bytes > kCopyBytes) continue;
        const uint32_t data = kernel::R32(st.vb + 4);
        if (!data) continue;
        const uint32_t va = mem::VirtualFromPhysical(data);
        if (!mem::IsCommitted(va) || !mem::IsCommitted(va + uint32_t(bytes) - 1)) continue;
        if (kept_bytes_.size() + bytes > kRegionBytes) continue;
        const auto* from = static_cast<const uint8_t*>(kernel::HostPtr(va));
        r.kept.push_back({uint8_t(k), uint32_t(kept_bytes_.size()), uint32_t(bytes)});
        kept_bytes_.insert(kept_bytes_.end(), from, from + bytes);
    }
}

// into memory the renderers can read, with a vertex buffer header for each; the region the step before used may still be being read
void Interpolating::UploadKept() {
    region_ ^= 1;
    if (!region_data_[region_]) {
        region_data_[region_] = mem::AllocHostContiguous(kRegionBytes, 0x1000);
        region_headers_[region_] = mem::AllocSystem(kRegionHeaders * 12);
    }
    inner_->WaitFence(region_fence_[region_]);
    if (!kept_bytes_.empty()) std::memcpy(kernel::HostPtr(region_data_[region_]), kept_bytes_.data(), kept_bytes_.size());
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
        const int candidate = it->second.back();
        it->second.pop_back();
        // HUD pieces of one picture look alike and their number changes: a different matrix is a different draw, or one that jumped
        const Rec& was = before_[size_t(candidate)];
        bool far = false;
        for (uint32_t k = 0; k < kMatrixRegisters && !far; k++)
            for (uint32_t c = 0; c < 4; c++) {
                const float x = was.vsc[k][c], y = r.vsc[k][c];
                if (std::fabs(x - y) > kFar * std::max({std::fabs(x), std::fabs(y), kFarLeast})) far = true;
            }
        matched++;
        if (far) {
            jumped++;
            continue;
        }
        r.prev = candidate;
        if (std::memcmp(before_[size_t(r.prev)].vsc.data(), r.vsc.data(), sizeof r.vsc)) moved++;
    }
    if (!moved || matched * 2 < draws) return false;
    if (jumped * 2 > matched) return false;  // a camera cut
    return true;
}

void Interpolating::Replay(float alpha) {
    const auto began = Clock::now();
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
                    if (header >= kRegionHeaders) break;
                    const uint32_t h = region_headers_[region_] + header++ * 12;
                    kernel::W32(h, 1);  // a vertex buffer, one reference
                    kernel::W32(h + 4, mem::PhysicalFromVirtual(region_data_[region_] + kept.offset));
                    kernel::W32(h + 8, 0);
                    d.streams[kept.stream].vb = h;
                }
                const bool blend = r.prev >= 0 && alpha < 1;
                if (blend) {
                    const Rec& p = before_[size_t(r.prev)];
                    for (uint32_t k = 0; k < kRegisters; k++)
                        for (uint32_t c = 0; c < 4; c++) d.vsc[k][c] = p.vsc[k][c] + (r.vsc[k][c] - p.vsc[k][c]) * alpha;
                } else {
                    d.vsc = r.vsc;
                }
                inner_->Draw(d, r.a, r.b, r.c, indices);
                d.streams = streams;
                break;
            }
        }
    }
    for (const Rec::Header& l : left) std::memcpy(kernel::HostPtr(l.va), l.bytes, HeaderSize(l.va));
    region_fence_[region_] = inner_->Fence();
    replay_seconds_ = std::chrono::duration<double>(Clock::now() - began).count();
}

void Interpolating::Present(const Surface& front) {
    const auto t = Clock::now();
    const double step = std::chrono::duration<double>(t - presented_).count();
    presented_ = t;
    const bool blend = On() && count_ && Match();
    if (blend) UploadKept();
    if (Test() && blend) {
        for (int i = 1; i < Test(); i++) {
            Replay(float(i) / float(Test()));
            inner_->Present(front);
        }
        Replay(1);
        inner_->Present(front);
    } else if (!blend || step > kLongestStep || step < kShortestStep) {
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
        // as many as fit: a replay's time, measured, falls back while none is drawn so it can be tried again
        const int fit = replay_seconds_ > 0 ? int(span / replay_seconds_) + 1 : 2;
        const int frames = std::clamp(std::min(int(usual / slot + 0.5), fit), 1, kMostFrames);
        // the picture follows the clock: a step's change is shown over the whole of the time to the next, which keeps the speed even.
        // The last frame stops short of the step, and the next step's first goes on from there
        const double spacing = std::min(slot, usual / frames);
        if (frames == 1) replay_seconds_ *= 0.9;
        if (frames > 1) {
            for (int i = 0; i < frames; i++) {
                const auto now = Clock::now();
                const double at = std::chrono::duration<double>(now - t).count();
                if (i && at >= span) break;
                Replay(float(std::min(at / usual, 1.0)));
                inner_->Present(front);
                if (i) shown_++;
                // not past the time the title needs for its next step
                const double until = std::min(at + spacing, span);
                std::this_thread::sleep_until(t + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(until)));
            }
        } else {
            inner_->Present(front);
        }
    }
    shown_++;
    steps_++;
    if (t - reported_ > std::chrono::seconds(10)) {
        if (reported_ != Clock::time_point{} && blended_steps_)
            LOGR("gpu: in-between frames: %u frames for %u steps (%u blended) in the last 10 s", shown_, steps_, blended_steps_);
        reported_ = t;
        shown_ = steps_ = blended_steps_ = 0;
    }
    now_.swap(before_);
    before_count_ = count_;
    count_ = 0;
    kept_bytes_.clear();
}
}  // namespace

std::unique_ptr<Backend> CreateInterpolating(std::unique_ptr<Backend> inner) { return std::make_unique<Interpolating>(std::move(inner)); }
}  // namespace gpu
