// headless renderer: counts frames and draws, logs draw lists, draws chosen frames with the reference renderer
// REF=N: every Nth frame drawn on the CPU into guest memory and saved as cap/ref_<frame>.tga
// CAP=N: every Nth frame's draws listed in cap/draws_<frame>.txt
// AUDIT=N: every Nth frame's shaders and textures as a GPU backend would get them, see audit.cpp
// snap in the run folder: the next two frames (SNAP_FRAMES) drawn and saved as snap_1.tga, snap_2.tga ..., then snap
// is removed
// memdump in the run folder: guest memory written to mem_<frame>.bin and mem_<frame>_phys.bin (uncommitted pages as
// zeros) within 30 frames, then memdump is removed
// poke in the run folder: lines "address size value" (hex address, size 1 2 or 4, value decimal or 0x hex) written to
// guest memory at the next present, then poke is removed (to meet a condition by hand: achievements)
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "app/app.h"
#include "audit.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "harness.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "reference_renderer.h"

namespace gpu {

namespace {

class NullBackend final : public Backend {
public:
    bool Init(uint32_t w, uint32_t h) override {
        LOGI("renderer: headless %ux%u", w, h);
        std::filesystem::create_directories(app::ExeDir() / "cap");
        return true;
    }
    void Clear(const State& s, uint32_t n, const uint8_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) override {
        if (drawing_) reference::Clear(s, n, rects, flags, color, z, stencil);
    }
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        draws_++;
        if (drawing_) reference::Draw(s, prim, start, count, indices);
        if (list_) List(s, prim, start, count);
        harness::AuditDraw(s, prim, start, count, indices);
    }
    uint64_t perf_frames_ = 0, perf_draws_ = 0;
    // XBE_CHECK_PRE=1: each draw the prep thread assembled is assembled again here, on the render thread, and compared
    void DrawAssembled(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices,
                       const gpu::Assembled& pre, bool ok) override {
        static const bool check = [] {
            const char* e = std::getenv("XBE_CHECK_PRE");
            return e && *e == '1';
        }();
        if (check) {
            static gpu::Assembled again;
            static uint64_t n = 0, wrong = 0;
            bool ok2 = gpu::Assemble(s, prim, start, count, indices, again);
            const char* why = SameAssembly(pre, ok, again, ok2);
            n++;
            if (why && wrong++ < 8) LOGR("prep check: draw %llu assembled differently (%s)", (unsigned long long)n, why);
            if (n % 200000 == 0)
                LOGR("prep check: %llu draws assembled ahead, %llu different", (unsigned long long)n, (unsigned long long)wrong);
        }
        Draw(s, prim, start, count, indices);
    }
    static const char* SameAssembly(const gpu::Assembled& a, bool oka, const gpu::Assembled& b, bool okb) {
        if (oka != okb) return "drawn or not";
        if (!oka) return nullptr;
        if (a.regs != b.regs || a.vertex_count != b.vertex_count || a.topology != b.topology || a.strip != b.strip ||
            a.pretransformed != b.pretransformed || a.decode != b.decode)
            return "layout";
        for (int r = 0; r < 16; r++)  // only the registers in use: Assemble leaves the others as they were
            if ((a.regs >> r) & 1 && std::memcmp(&a.elements[size_t(r)], &b.elements[size_t(r)], sizeof(a.elements[0])))
                return "elements";
        if (a.buffer_count != b.buffer_count) return "buffer count";
        for (uint32_t i = 0; i < a.buffer_count; i++) {
            if (a.buffers[i].bytes != b.buffers[i].bytes || a.buffers[i].stride != b.buffers[i].stride) return "buffer size";
            if (std::memcmp(a.buffers[i].src, b.buffers[i].src, a.buffers[i].bytes)) return "vertex bytes";
        }
        if (a.index_count != b.index_count || a.draw_count != b.draw_count || a.base_vertex != b.base_vertex ||
            a.index16 != b.index16)
            return "draw";
        if (a.index_count && std::memcmp(a.index_src, b.index_src, size_t(a.index_count) * (a.index16 ? 2 : 4)))
            return "indices";
        return nullptr;
    }
    void Copy(const Surface& src, const Surface& dst, uint32_t n, uint32_t rects, uint32_t points) override {
        if (drawing_) reference::Copy(src, dst, n, rects, points);
        else CopySurfaceMemory(src, dst, n, rects, points);
    }
    void Present(const Surface& front) override {
        // XBE_PERF with the render thread on: its own times, every 600 frames
        static const bool perf = std::getenv("XBE_PERF") != nullptr;
        if (perf && ++perf_frames_ % 600 == 0) {
            uint64_t d = draws_ - perf_draws_;
            perf_draws_ = draws_;
            LOGR("perf: render thread per frame: state %.3f ms, batches %.3f ms, prep %.3f ms, %.0f draws",
                 gpu::g_perf_state_ns.exchange(0) / 600.0 / 1e6, gpu::g_perf_batch_ns.exchange(0) / 600.0 / 1e6,
                 gpu::g_perf_prep_ns.exchange(0) / 600.0 / 1e6, d / 600.0);
        }
        if (list_) {
            std::fclose(list_);
            list_ = nullptr;
        }
        if (draws_ == last_draws_) empty_++;
        last_draws_ = draws_;
        if (drawing_ && !snap_) reference::WriteTga(front, File("cap/ref_%05llu.tga", frames_).c_str());
        // snap asks for the next frames drawn: two, or SNAP_FRAMES
        std::string snap = (app::ExeDir() / "snap").string();
        static const long snaps = std::max(2L, harness::Env("SNAP_FRAMES", 2));
        if (snap_) {
            std::string name = "snap_" + std::to_string(snap_) + ".tga";
            reference::WriteTga(front, (app::ExeDir() / name).string().c_str());
            if (snap_ < snaps) snap_++;
            else {
                std::remove(snap.c_str());
                snap_ = 0;
            }
        } else if (FILE* f = std::fopen(snap.c_str(), "r")) {
            std::fclose(f);
            snap_ = 1;
        }
        if (std::FILE* f = std::fopen((app::ExeDir() / "poke").string().c_str(), "r")) {
            unsigned addr, size;
            char value[32];
            while (std::fscanf(f, "%x %u %31s", &addr, &size, value) == 3) {
                uint32_t v = uint32_t(std::strtoul(value, nullptr, 0));
                if (size == 1) *static_cast<uint8_t*>(kernel::HostPtr(addr)) = uint8_t(v);
                else if (size == 2) kernel::W16(addr, uint16_t(v));
                else kernel::W32(addr, v);
                LOGI("poke: %08X = %u (%u bytes)", addr, v, size);
            }
            std::fclose(f);
            std::remove((app::ExeDir() / "poke").string().c_str());
        }
        std::string memdump = (app::ExeDir() / "memdump").string();
        if (frames_ % 30 == 0 && std::filesystem::exists(memdump)) {
            std::remove(memdump.c_str());
            DumpMemory();
        }
        static const long every = harness::Env("REF"), cap = harness::Env("CAP");
        drawing_ = snap_ || (every && (frames_ + 1) % every == 0);
        if (drawing_) reference::BeginFrame();
        harness::g_frames = frames_ + 1;
        harness::AuditPresent();
        if (++frames_ % 60 == 0) LOGI("renderer: frame %llu, %llu draws, %llu empty frames, vblank %llu", frames_, draws_, empty_,
                                         (unsigned long long)harness::g_vblanks.load());
        if (cap && frames_ % cap == 0) list_ = std::fopen(File("cap/draws_%05llu.txt", frames_).c_str(), "w");
    }
    void Sync(uint32_t, uint32_t) override {}
    void SyncLate(uint32_t) override {}
    void Flush() override {
        for (uint32_t report : held_) WriteReport(report);
        held_.clear();
    }
    void SetGamma(const uint8_t*) override {}
    // visibility reports are written at once as the console's GPU writes them: time, count, then 0 for done. Nothing
    // is rasterised here between reference frames, so no pixel is counted.
    // HOLD_REPORTS=1: held until a flush, as a GPU backend delivers them: a title polling a report without one hangs
    void GetReport(uint32_t report) override {
        static const bool hold = std::getenv("HOLD_REPORTS") != nullptr;
        if (hold) held_.push_back(report);
        else WriteReport(report);
    }
    void WriteReport(uint32_t report) {
        constexpr uint32_t kTime = 0, kTimeHigh = 4, kCount = 8, kStatus = 12;
        kernel::W32(report + kTime, uint32_t(frames_));
        kernel::W32(report + kTimeHigh, 0);
        kernel::W32(report + kCount, 0);
        kernel::W32(report + kStatus, 0);
    }

private:
    std::vector<uint32_t> held_;
    // the user space's first 128 MB to mem_<frame>.bin, the contiguous mapping of physical memory (0x80000000, 64 MB)
    // to mem_<frame>_phys.bin
    void DumpMemory() {
        constexpr uint32_t kPage = 4096;
        static const std::vector<uint8_t> zeros(kPage);
        struct Range {
            uint32_t base, size;
            const char* name;
        };
        for (Range r : {Range{0, 128u << 20, "mem_%05llu.bin"}, Range{0x80000000u, 64u << 20, "mem_%05llu_phys.bin"}}) {
            std::string name = File(r.name, frames_);
            FILE* f = std::fopen(name.c_str(), "wb");
            if (!f) continue;
            for (uint32_t va = r.base; va - r.base < r.size; va += kPage)
                std::fwrite(mem::IsCommitted(va) ? kernel::HostPtr(va) : zeros.data(), 1, kPage, f);
            std::fclose(f);
            LOGI("memdump: guest memory %08X+%X written to %s", r.base, r.size, name.c_str());
        }
    }

    static std::string File(const char* fmt, unsigned long long n) {
        char name[64];
        std::snprintf(name, sizeof name, fmt, n);
        return (app::ExeDir() / name).string();
    }

    // one draw's state, textures, transforms, declaration and first vertex
    void List(const State& s, uint32_t prim, uint32_t start, uint32_t count) {
        FILE* f = list_;
        std::fprintf(f, "DRAW prim=%u n=%u vs=%X ps=%X rt=%08X zs=%08X tex0=%08X light=%u blend=%u %X %X cw=%X at=%u af=%X ar=%X z=%u zf=%X vp=%u,%u,%ux%u\n", prim, count,
                     s.vs_handle, s.ps ? s.ps->handle : 0, s.rt.data, s.zs.data, s.textures[0], s.rs[kRsLighting], s.simple[kMBlendEnable >> 2],
                     s.simple[kMSrcBlend >> 2], s.simple[kMDestBlend >> 2], s.simple[kMColorWriteEnable >> 2],
                     s.simple[kMAlphaTestEnable >> 2], s.simple[kMAlphaFunc >> 2], s.simple[kMAlphaRef >> 2], s.rs[kRsZEnable],
                     s.simple[kMZFunc >> 2],
                     s.viewport.x, s.viewport.y, s.viewport.w, s.viewport.h);
        if (s.scissor_count) {  // the title's scissor rectangles (SetScissors)
            std::fprintf(f, "  scissors %u:", s.scissor_count);
            for (uint32_t i = 0; i < s.scissor_count && i < 8; i++)
                std::fprintf(f, " %u,%u-%u,%u", s.scissors[i * 4], s.scissors[i * 4 + 1], s.scissors[i * 4 + 2], s.scissors[i * 4 + 3]);
            std::fprintf(f, "\n");
        }
        const auto& t0 = s.tss[0];
        TextureDesc d;
        if (s.textures[0] && Describe(s.textures[0], d))
            std::fprintf(f, "  tex %ux%u fmt %X data %08X cop %u %X %X aop %u %X %X\n", d.width, d.height, d.format, d.data, t0[kTssColorOp],
                         t0[kTssColorArg1], t0[kTssColorArg2], t0[kTssAlphaOp], t0[kTssAlphaArg1], t0[kTssAlphaArg2]);
        else  // no texture: the stage's operations still say what the draw writes
            std::fprintf(f, "  no texture cop %u %X %X aop %u %X %X tfactor %08X\n", t0[kTssColorOp], t0[kTssColorArg1], t0[kTssColorArg2],
                         t0[kTssAlphaOp], t0[kTssAlphaArg1], t0[kTssAlphaArg2], s.rs[kRsTextureFactor]);
        // the other stages: their texture, address modes, border colour and filters (projected shadow maps and such)
        for (int i = 1; i < 4; i++) {
            if (!s.textures[i]) continue;
            const auto& t = s.tss[i];
            TextureDesc di;
            const bool described = Describe(s.textures[i], di);
            std::fprintf(f, "  stage %d tex %08X %ux%u fmt %X addr %u %u border %08X filter %u %u ttf %X tci %X\n", i, s.textures[i],
                         described ? di.width : 0, described ? di.height : 0, described ? di.format : 0, t[kTssAddressU],
                         t[kTssAddressV], t[kTssBorderColor], t[kTssMagFilter], t[kTssMinFilter], t[kTssTextureTransformFlags],
                         t[kTssTexCoordIndex]);
        }
        std::fprintf(f, "  ttf %X tci %X addr %u %u border %08X t16", t0[kTssTextureTransformFlags], t0[kTssTexCoordIndex],
                     t0[kTssAddressU], t0[kTssAddressV], t0[kTssBorderColor]);
        auto matrix = [&](const char* name, uint32_t which) {
            std::fprintf(f, "%s", name);
            for (float v : s.transforms[which]) std::fprintf(f, " %g", v);
        };
        matrix("", 16);
        matrix("\n  world", 256);
        matrix("\n  view", 2);
        matrix("\n  proj", 3);
        std::fprintf(f, "\n");
        if (s.vs) {
            std::fprintf(f, "  decl");
            for (int r = 0; r < 16; r++) {
                const auto& in = s.vs->inputs[size_t(r)];
                if (in.stream >= 0) std::fprintf(f, " v%d:%d+%u/%X", r, in.stream, in.offset, in.type);
            }
            std::fprintf(f, "\n");
        }
        if (s.streams[0].vb) {
            // the vertex buffer's Data at +4 is a physical address
            // the first vertex; each of a small draw's (6 or fewer: a quad's corners)
            uint32_t data = mem::VirtualFromPhysical(kernel::R32(s.streams[0].vb + 4)) + start * s.streams[0].stride;
            for (uint32_t n = 0; n < (count <= 6 ? count : 1); n++) {
                std::fprintf(f, n ? "  v%u:" : "  v0 stride %u:", n ? n : s.streams[0].stride);
                for (uint32_t i = 0; i < s.streams[0].stride / 4 && i < 10; i++) {
                    uint32_t v = kernel::R32(data + n * s.streams[0].stride + i * 4);
                    float x;
                    std::memcpy(&x, &v, 4);
                    std::fprintf(f, " %g(%08X)", x, v);
                }
                std::fprintf(f, "\n");
            }
        }
    }

    unsigned long long frames_ = 0, draws_ = 0, last_draws_ = 0, empty_ = 0;
    bool drawing_ = false;
    int snap_ = 0;
    FILE* list_ = nullptr;
};

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<NullBackend>(); }
std::unique_ptr<Backend> CreateFallbackBackend() { return nullptr; }

}  // namespace gpu
