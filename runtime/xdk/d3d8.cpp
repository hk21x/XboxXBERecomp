// Direct3D 8 (XDK 5849) entry points the title calls, implemented on the host renderer.
// Resource creation, locking and the XG helpers stay recompiled: they only manage memory.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "app/app.h"
#include "core/achievements.h"
#include "core/diag.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"
#include "xdk/layout.h"
#include "xdk/state_numbering.h"

using namespace kernel;

// configs written before the SDK knew the name
#ifndef SYM_D3DDevice_BeginPush
#define SYM_D3DDevice_BeginPush FFFFFE00
#define HAS_D3DDevice_BeginPush 0
#endif
#ifndef SYM_D3DDevice_EndPush
#define SYM_D3DDevice_EndPush FFFFFE01
#define HAS_D3DDevice_EndPush 0
#endif
#ifndef HAS_D3D_FvfShader
#define HAS_D3D_FvfShader 0
#endif
#ifndef HAS_D3D_PresentInterval
#define SYM_D3D_PresentInterval 0
#define HAS_D3D_PresentInterval 0
#endif
#ifndef SYM_D3DDevice_SetPalette
#define SYM_D3DDevice_SetPalette FFFFFE02
#define HAS_D3DDevice_SetPalette 0
#endif
#ifndef SYM_D3DDevice_Present
#define SYM_D3DDevice_Present FFFFFE09
#define HAS_D3DDevice_Present 0
#endif
#ifndef SYM_D3DDevice_GetBackBuffer
#define SYM_D3DDevice_GetBackBuffer FFFFFE0A
#define HAS_D3DDevice_GetBackBuffer 0
#endif
#ifndef SYM_D3DDevice_GetDepthStencilSurface
#define SYM_D3DDevice_GetDepthStencilSurface FFFFFE0B
#define HAS_D3DDevice_GetDepthStencilSurface 0
#endif
#ifndef SYM_D3DDevice_SetVertexShaderConstant
#define SYM_D3DDevice_SetVertexShaderConstant FFFFFE0C
#define HAS_D3DDevice_SetVertexShaderConstant 0
#endif
#ifndef SYM_D3DDevice_GetRasterStatus
#define SYM_D3DDevice_GetRasterStatus FFFFFE13
#define HAS_D3DDevice_GetRasterStatus 0
#endif
#ifndef SYM_D3DDevice_BeginPushBuffer
#define SYM_D3DDevice_BeginPushBuffer FFFFFE11
#define HAS_D3DDevice_BeginPushBuffer 0
#endif
#ifndef SYM_D3DDevice_EndPushBuffer
#define SYM_D3DDevice_EndPushBuffer FFFFFE12
#define HAS_D3DDevice_EndPushBuffer 0
#endif
#ifndef SYM_D3DDevice_RunPushBuffer
#define SYM_D3DDevice_RunPushBuffer FFFFFE0D
#define HAS_D3DDevice_RunPushBuffer 0
#endif
#ifndef SYM_D3DDevice_GetVertexShaderFunction
#define SYM_D3DDevice_GetVertexShaderFunction FFFFFE03
#define HAS_D3DDevice_GetVertexShaderFunction 0
#endif
#ifndef SYM_D3DDevice_Reset
#define SYM_D3DDevice_Reset FFFFFE04
#define HAS_D3DDevice_Reset 0
#endif
#ifndef SYM_D3DDevice_SetRenderTargetFast
#define SYM_D3DDevice_SetRenderTargetFast FFFFFE06
#define HAS_D3DDevice_SetRenderTargetFast 0
#endif
#ifndef SYM_D3DDevice_SetVertexShaderInput
#define SYM_D3DDevice_SetVertexShaderInput FFFFFE07
#define HAS_D3DDevice_SetVertexShaderInput 0
#endif
#ifndef SYM_D3DDevice_SetVertexShaderInputDirect
#define SYM_D3DDevice_SetVertexShaderInputDirect FFFFFE08
#define HAS_D3DDevice_SetVertexShaderInputDirect 0
#endif
#ifndef SYM_D3DDevice_SelectVertexShaderDirect
#define SYM_D3DDevice_SelectVertexShaderDirect FFFFFE10
#define HAS_D3DDevice_SelectVertexShaderDirect 0
#endif
#ifndef SYM_D3DDevice_SetBackBufferScale
#define SYM_D3DDevice_SetBackBufferScale FFFFFE0F
#define HAS_D3DDevice_SetBackBufferScale 0
#endif
#ifndef SYM_D3DDevice_UpdateOverlay
#define SYM_D3DDevice_UpdateOverlay FFFFFE0E
#define HAS_D3DDevice_UpdateOverlay 0
#endif
#ifndef SYM_D3DDevice_SetDepthClipPlanes
#define SYM_D3DDevice_SetDepthClipPlanes FFFFFE05
#define HAS_D3DDevice_SetDepthClipPlanes 0
#endif

namespace d3d {

namespace {

// A/B switch: 0 draws on the title's thread, as before; the environment variable XBE_RENDER_THREAD=0 or 1
// overrides the build's choice
#ifndef XBE_RENDER_THREAD
#define XBE_RENDER_THREAD 1
#endif
bool RenderThread() {
    static const bool on = [] {
        const char* e = std::getenv("XBE_RENDER_THREAD");
        bool v = e && *e ? *e != '0' : XBE_RENDER_THREAD;
        if (e && *e) LOGR("d3d: render thread %s (XBE_RENDER_THREAD)", v ? "on" : "off");
        return v;
    }();
    return on;
}

// D3D globals of this build
constexpr uint32_t kDevice = SYM(D3D_Device);        // the static CDevice, g_pDevice points at it
constexpr uint32_t kDevicePtr = SYM(D3D_pDevice);
constexpr uint32_t kIndexData = SYM(D3D_IndexData);  // index data base the inline draws read
constexpr uint32_t kDirtyFlags = SYM(D3D_DirtyFlags);
constexpr uint32_t kTextureState = SYM(D3D_DeferredTextureState);  // [4][32]
constexpr uint32_t kRenderState = SYM(D3D_RenderState);            // [states::kNumbering.count]
constexpr uint32_t kStreams = SYM(D3D_Streams);                    // [16] stride, offset, buffer
constexpr uint32_t kSimpleEncode = SYM(D3D_SimpleRenderStateEncode);

// CDevice fields mirrored so the recompiled getters and resource code see what the XDK would
constexpr uint32_t kDevPush = XDK_CDevice_Push, kDevThreshold = XDK_CDevice_PushThreshold,
                   kDevBaseVertex = XDK_CDevice_BaseVertex, kDevIndexBuffer = XDK_CDevice_IndexBuffer,
                   kDevTextures = XDK_CDevice_Textures, kDevRenderTarget = XDK_CDevice_RenderTarget,
                   kDevDepth = XDK_CDevice_Depth, kDevSwapCount = XDK_CDevice_SwapCount,
                   kDevBackBuffer = XDK_CDevice_BackBuffer, kDevFrontBuffer = XDK_CDevice_FrontBuffer,
                   kDevZScale = XDK_CDevice_ZScale, kDevSuperX = XDK_CDevice_SuperSampleX,
                   kDevSuperY = XDK_CDevice_SuperSampleY, kDevTransforms = XDK_CDevice_Transforms,
                   kDevPalettes = XDK_CDevice_Palettes;
constexpr uint32_t kStageStates = XDK_TextureState_Stage, kStreamSize = XDK_Stream_Size;

gpu::State g_state;
std::unique_ptr<gpu::Backend> g_backend;
std::recursive_mutex g_lock;

std::unordered_map<uint32_t, std::shared_ptr<gpu::VertexShader>> g_vs;
// the GPU's vertex program memory: 136 slots of one instruction, loaded by pushed commands (0x1E9C the slot to load
// from, 0x0B00 to 0x0B7C the instructions, as LoadVertexShaderProgram pushes them) or by LoadVertexShader
constexpr uint32_t kProgramSlots = 136;
uint32_t g_program[kProgramSlots * 4] = {};
uint32_t g_program_load = 0;  // the next dword written
// the slot SelectVertexShader started the bound program at (-1: a created shader), and program memory written since:
// the console runs program memory as it is when it draws, the title may load the program after selecting it
int32_t g_program_slot = -1;
bool g_program_written = false;
std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> g_ps;
std::array<uint32_t, 3> g_buffers{};  // front, back surfaces
uint32_t g_buffer_count = 2;
uint32_t g_depth = 0;
// Swap with the copy flags and not FINISH starts the title's own copy of the back buffer to the front buffer (a title
// may draw it as a quad, downsampling a supersampled back buffer). The host renders at the back buffer's own size and
// shows it as it is, so that copy is drawn into this target, never shown, until FINISH.
uint32_t g_copy_target = 0;
bool g_copy_open = false;
uint32_t g_scratch_push = 0, g_scratch_size = 0;
uint32_t g_walk = 0;        // first pushed command not read yet
uint32_t g_walk_limit = 0;  // end of the buffer it is in: the scratch buffer, or a push buffer being recorded
bool g_report_asked = false;  // a visibility report asked for since the last flush

uint32_t Dev(uint32_t off) { return R32(kDevice + off); }
void SetDev(uint32_t off, uint32_t v) { W32(kDevice + off, v); }

// The device's own references: a count in bits 19 to 22 of the resource's first dword. A surface's first one
// also holds its parent. The library's functions do it when the image has them, inlined ones are done here.
void AddRef(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalAddRef
    GUEST_CALL(D3D_InternalAddRef, ctx, {res});
#else
    (void)ctx;
    uint32_t common = R32(res);
    if (!(common & 0x780000) && (common & 0x70000) == 0x50000)
        if (uint32_t parent = R32(res + 0x14)) W32(parent, R32(parent) + 0x80000);
    W32(res, R32(res) + 0x80000);
#endif
}
void Release(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalRelease
    GUEST_CALL(D3D_InternalRelease, ctx, {res});
#else
    (void)ctx;
    // the library frees a resource nothing holds any more; without its function the memory stays
    auto drop = [](uint32_t r) {
        W32(r, R32(r) - 0x80000);
        if (!(R32(r) & 0x78FFFF)) LOG_FIRST(8, "d3d: resource %08X is not freed, the image has no D3D_InternalRelease", r);
    };
    uint32_t common = R32(res);
    if ((common & 0x780000) == 0x80000 && (common & 0x70000) == 0x50000)
        if (uint32_t parent = R32(res + 0x14)) drop(parent);
    drop(res);
#endif
}

void Walk(uint32_t end);

// Hardware commands a caller still pushes go to a scratch buffer, rewound at every request once Walk has read it
uint32_t ScratchPush(uint32_t bytes = 0) {
    Walk(Dev(kDevPush));
    uint32_t need = std::max<uint32_t>(0x10000, (bytes + 0x1FFF) & ~0xFFFu);
    if (need > g_scratch_size) {
        if (g_scratch_push) mem::FreeContiguous(g_scratch_push);
        g_scratch_push = mem::AllocHostContiguous(need, 0x1000);
        g_scratch_size = need;
    }
    SetDev(kDevPush, g_scratch_push);
    SetDev(kDevThreshold, g_scratch_push + g_scratch_size - 0x1000);
    g_walk = g_scratch_push;
    g_walk_limit = g_scratch_push + g_scratch_size;
    return g_scratch_push;
}

gpu::Surface SurfaceOf(uint32_t header) {
    gpu::Surface s;
    s.header = header;
    if (header) {
        s.data = R32(header + 4);
        s.format = R32(header + 12);
        s.size = R32(header + 16);
    }
    return s;
}

// hdr: where the surface object goes, 0 for a new one; keep: a title's own surface there keeps its memory when it
// holds this one (the title may find that memory again another way: 007: Everything or Nothing's GetTile)
// host: memory the title never sees (mem::AllocHostContiguous), for the renderer's own surfaces
uint32_t NewSurface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t bpp, uint32_t hdr = 0, bool keep = false, bool host = false) {
    uint32_t pitch = (w * bpp + 63) & ~63u;
    uint32_t phys = 0;
    if (keep && hdr && R32(hdr + 4) && R32(hdr + 16)) {
        uint32_t size = R32(hdr + 16), had = ((size >> 24) + 1) * 64 * (((size >> 12) & 0xFFF) + 1);
        if (had >= pitch * h) phys = R32(hdr + 4) & 0x0FFFFFFF;
    }
    if (!phys)
        phys = mem::PhysicalFromVirtual(host ? mem::AllocHostContiguous(pitch * h, 0x1000)
                                             : mem::AllocContiguous(pitch * h, 0, 0xFFFFFFFF, 0x1000));
    if (!hdr) hdr = mem::AllocSystem(24);
    W32(hdr + 0, 0x000D0001);  // surface, one reference, one internal reference held by the device
    W32(hdr + 4, phys);
    W32(hdr + 8, 0);
    W32(hdr + 12, 0x00010021 | (fmt << 8));  // dma A, 2D, one level
    W32(hdr + 16, (w - 1) | (h - 1) << 12 | (pitch / 64 - 1) << 24);
    W32(hdr + 20, 0);
    return hdr;
}

void SyncDeferred() {
    // deferred states live in the title's memory, the inline SetRenderState and SetTextureStageState write them
    const auto& n = xdk::states::kNumbering;
    for (uint32_t i = gpu::kRsDeferredFirst; i < gpu::kRsComplexFirst; i++)
        if (n.rs[i] != xdk::states::kNone) g_state.rs[i] = R32(kRenderState + n.rs[i] * 4);
    for (int s = 0; s < 4; s++)
        for (uint32_t i = 0; i < gpu::kTssBumpEnvMat00; i++)
            g_state.tss[s][i] = R32(kTextureState + (s * kStageStates + n.tss[i]) * 4);
}

float ZScale(const gpu::Surface& zs) {
    uint32_t f = zs.header ? (zs.format >> 8) & 0xFF : 0x2E;
    return f == 0x2C || f == 0x30 ? 65535.0f : 16777215.0f;
}

bool g_own_constants = false;  // D3DSCM_NORESERVEDCONSTANTS: c-38 and c-37 are the title's

// c-38 and c-37 hold the viewport scale and offset, as D3D_GetViewportOffsetAndScale computes them
void UpdateViewportConstants() {
    auto& v = g_state.viewport;
    g_state.zscale = ZScale(g_state.zs);
    float hw = v.w * 0.5f, hh = v.h * 0.5f;
    if (!g_own_constants) {
        g_state.vsc[58] = {hw, -hh, (v.max_z - v.min_z) * g_state.zscale, 0};
        g_state.vsc[59] = {v.x + g_state.ss_offset_x + hw, v.y + g_state.ss_offset_y + hh, v.min_z * g_state.zscale, 0};
    }
    // the viewport as the library's SetViewport stores it, for inlined readers
    if (XDK_CDevice_Viewport != xdk::kUnknown) {
        uint32_t p = kDevice + XDK_CDevice_Viewport;
        W32(p, v.x);
        W32(p + 4, v.y);
        W32(p + 8, v.w);
        W32(p + 12, v.h);
        std::memcpy(HostPtr(p + 16), &v.min_z, 4);
        std::memcpy(HostPtr(p + 20), &v.max_z, 4);
    }
}

// the back buffer's samples across and down, from the D3DPRESENT_PARAMETERS MultiSampleType (0x2021, 2 across)
uint32_t g_made_samples[2] = {1, 1};

// State::aa_x, aa_y: D3DRS_MULTISAMPLEMODE (on the back buffer) or _RENDERTARGETMODE (on another target) asking for
// more samples than the target was made with: D3DMULTISAMPLEMODE_1X 0, _2X 1 (the made buffer's way, else across),
// _4X 2 (2 by 2). Fewer, as a supersampled buffer drawn 1X, keep 1: the renderer makes the buffer its drawn size.
// Any of the frame buffers counts as the back buffer: they take turns at it (EoN's second mission draws its frames
// into its two buffers in turn, through both surface objects; only the back buffer's drew particles in place).
void UpdateSampleScale() {
    bool back = false;
    for (uint32_t i = 0; i < g_buffer_count; i++) back |= g_state.rt.header && g_state.rt.header == g_buffers[i];
    uint32_t made_x = back ? g_made_samples[0] : 1, made_y = back ? g_made_samples[1] : 1;
    uint32_t mode = g_state.rs[back ? gpu::kRsMultiSampleMode : gpu::kRsMultiSampleRenderTargetMode];
    uint32_t x = 1, y = 1;
    if (mode == 2) x = y = 2;
    else if (mode == 1) (made_y == 2 && made_x == 1 ? y : x) = 2;
    g_state.aa_x = std::max<uint32_t>(x / made_x, 1);
    g_state.aa_y = std::max<uint32_t>(y / made_y, 1);
    // each target the back buffer's mode is asked on, once: applied (the back buffer) or not (another target)
    uint32_t asked = g_state.rs[gpu::kRsMultiSampleMode];
    if (asked && g_state.rt.header) {
        static std::unordered_set<uint32_t> told;
        if (told.size() < 16 && told.insert(g_state.rt.header).second) {
            uint32_t w, h;
            gpu::SurfaceSize(g_state.rt, w, h);
            LOGR("d3d: MultiSampleMode %u with target %08X (%ux%u, data %08X)%s: drawn %ux%u to a pixel", asked,
                 g_state.rt.header, w, h, g_state.rt.data, back ? ", a frame buffer" : ", not a frame buffer",
                 g_state.aa_x, g_state.aa_y);
        }
    }
}

void SetComplex(uint32_t rs, uint32_t v) {
    g_state.rs[rs] = v;
    if (uint32_t t = xdk::states::kNumbering.rs[rs]; t != xdk::states::kNone) W32(kRenderState + t * 4, v);
    if (rs == gpu::kRsMultiSampleMode || rs == gpu::kRsMultiSampleRenderTargetMode) UpdateSampleScale();
}

// a texture stage state as the library stores it, in the title's numbering
void StoreTss(uint32_t stage, uint32_t tss, uint32_t v) {
    g_state.tss[stage][tss] = v;
    W32(kTextureState + (stage * kStageStates + xdk::states::kNumbering.tss[tss]) * 4, v);
}

// The library's default render and texture stage states; its pushes land in the scratch buffer
void ApplyDefaultStates(X86Context& ctx) {
#if HAS_D3D_InitDefaultState
    GUEST_CALL(D3D_InitDefaultState, ctx, {});
#else
    (void)ctx;
    LOG_FIRST(1, "d3d: the image has no D3D_InitDefaultState, default states were not applied");
#endif
}

constexpr bool kVBlank = XDK_CDevice_VBlankCount != xdk::kUnknown && XDK_CDevice_VBlankEvent != xdk::kUnknown &&
                         XDK_CDevice_VBlankCallback != xdk::kUnknown;

// when the last vertical blank began (steady clock nanoseconds), for the raster position
std::atomic<int64_t> g_vblank_time{0};

// what the library's vertical blank interrupt leaves for the title: count, event, callback
void VerticalBlank(X86Context& ctx) {
    using clock = std::chrono::steady_clock;
    uint32_t info = mem::AllocSystem(12), swap_data = mem::AllocSystem(20), shown = 0;
    auto next = clock::now();
    for (;;) {
        next = std::max(next + std::chrono::microseconds(16667), clock::now());
        std::this_thread::sleep_until(next);
        uint32_t count = Dev(XDK_CDevice_VBlankCount) + 1, swaps = Dev(kDevSwapCount);
        SetDev(XDK_CDevice_VBlankCount, count);
        g_vblank_time = clock::now().time_since_epoch().count();
        ObjectRef event = FromPointer(kDevice + XDK_CDevice_VBlankEvent);
        SetEvent(*event);
        if (uint32_t callback = Dev(XDK_CDevice_VBlankCallback); callback && XDK_VBlankCallback_Count) {
            CallGuest(ctx, callback, {count});
        } else if (callback) {
            // blank count, swaps shown, 1 when this blank showed one
            W32(info, count);
            W32(info + 4, swaps);
            W32(info + 8, swaps != shown);
            CallGuest(ctx, callback, {info});
        }
        // a swap shown at this blank: the swap callback gets D3DSWAPDATA (Swap, SwapVBlank, MissedVBlanks,
        // TimeUntilSwapVBlank, TimeBetweenSwapVBlanks); titles timestamp their frames there
        if (XDK_CDevice_SwapCallback != xdk::kUnknown && swaps != shown)
            if (uint32_t callback = Dev(XDK_CDevice_SwapCallback)) {
                W32(swap_data, swaps);
                W32(swap_data + 4, count);
                W32(swap_data + 8, 0);
                W32(swap_data + 12, 0);
                W32(swap_data + 16, 0);
                CallGuest(ctx, callback, {swap_data});
            }
        shown = swaps;
        // the title clears the event in its own memory before a wait, which the host object does not see
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ResetEvent(*event);
    }
}

}  // namespace

gpu::State& State() { return g_state; }
std::recursive_mutex& Lock() { return g_lock; }

}  // namespace d3d

void WalkPending();  // below, with the vertex shader constants
void WalkBeforeState();

using namespace d3d;

// device

namespace {
// a surface object the title made and put in D3DPRESENT_PARAMETERS (BufferSurfaces, DepthStencilSurface), else 0
uint32_t GivenSurface(uint32_t at) {
    uint32_t s = R32(at);
    return s && mem::IsCommitted(s) && mem::IsCommitted(s + 23) && (R32(s) >> 16 & 7) == 5 ? s : 0;
}

// the swap chain and auto depth buffer a D3DPRESENT_PARAMETERS at pp asks for, made the device's targets. The same
// parameters again keep the buffers, so surfaces a title still holds stay valid; true when they are new. Frame
// buffers the title made itself (BufferSurfaces, back buffer first, and DepthStencilSurface) are made over: their
// objects become the buffers, in this size and format (a supersampled size is not kept: the renderer scales instead).
bool MakeFrameBuffers(uint32_t pp) {
    uint32_t w = R32(pp), h = R32(pp + 4), fmt = R32(pp + 8), count = std::max<uint32_t>(R32(pp + 12), 1);
    uint32_t auto_depth = R32(pp + 0x20), depth_fmt = R32(pp + 0x24), samples = R32(pp + 0x10);
    g_made_samples[0] = std::max<uint32_t>(samples >> 4 & 0xF, 1);
    g_made_samples[1] = std::max<uint32_t>(samples & 0xF, 1);
    // older XDKs' parameters end before these fields (3948's after the presentation interval, 0x34 bytes): what
    // follows there is the caller's stack, which once named game code a back buffer
    constexpr uint32_t kGiven = XDK_PresentParams_BufferSurfaces;
    uint32_t given[3] = {}, given_depth = 0;
    if (kGiven != xdk::kUnknown) {
        for (uint32_t i = 0; i < 3; i++) given[i] = GivenSurface(pp + kGiven + i * 4);
        given_depth = GivenSurface(pp + kGiven + 12);
    }
    if (given_depth && !auto_depth) depth_fmt = R32(given_depth + 12) >> 8 & 0xFF;
    if (!w) w = 640;
    if (!h) h = 480;
    static uint32_t made[7];
    uint32_t want[7] = {w, h, fmt, count, auto_depth || given_depth ? depth_fmt : 0xFFFFFFFFu, given[0], given_depth};
    bool fresh = !g_buffers[1] || std::memcmp(made, want, sizeof made);
    if (fresh) {
        uint32_t color_fmt = gpu::LinearColorFormat(fmt);
        g_buffer_count = std::min<uint32_t>(count + 1, 3);
        // in the device (3948) the back buffer is slot 0 and the front slot 1, as its GetBackBuffer counts them;
        // BufferSurfaces count the same way
        constexpr uint32_t kSlot[3] = {1, 0, 2};
        for (uint32_t i = 0; i < g_buffer_count; i++)
            g_buffers[i] = NewSurface(w, h, color_fmt, 4,
                                      given[kSlot[i]]                              ? given[kSlot[i]]
                                      : XDK_CDevice_FrameSurfaces == xdk::kUnknown ? 0
                                                                                   : kDevice + XDK_CDevice_FrameSurfaces + kSlot[i] * 24,
                                      given[kSlot[i]] != 0);
        g_copy_target = NewSurface(w, h, color_fmt, 4, 0, false, true);
        g_depth = 0;
        if (auto_depth || given_depth) {
            uint32_t zf = gpu::LinearDepthFormat(depth_fmt);
            g_depth = NewSurface(w, h, zf, zf == 0x30 || zf == 0x31 ? 2 : 4,  // 0x30 LIN_D16, 0x31 LIN_F16
                                 given_depth                                     ? given_depth
                                 : XDK_CDevice_AutoDepthSurface == xdk::kUnknown ? 0
                                                                                 : kDevice + XDK_CDevice_AutoDepthSurface,
                                 given_depth != 0);
        }
        std::memcpy(made, want, sizeof made);
    }
    SetDev(kDevRenderTarget, g_buffers[1]);
    SetDev(kDevDepth, g_depth);
    if (XDK_CDevice_FrameSurfaces == xdk::kUnknown) {
        SetDev(kDevBackBuffer, g_buffers[1]);
        SetDev(kDevFrontBuffer, g_buffers[1]);
    }
    float one = 1.0f, zs = g_depth && (R32(g_depth + 12) >> 8 & 0xFF) == 0x30 ? 65535.0f : 16777215.0f;
    std::memcpy(HostPtr(kDevice + kDevSuperX), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevSuperY), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevZScale), &zs, 4);
    // fields the library's init sets that inlined library code and the default states read
    if (XDK_CDevice_AutoDepth != xdk::kUnknown) SetDev(XDK_CDevice_AutoDepth, g_depth);
    if (XDK_CDevice_BackBufferScale != xdk::kUnknown) {
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale), &one, 4);
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale + 4), &one, 4);
    }
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    g_state.rt = SurfaceOf(g_buffers[1]);
    g_state.zs = SurfaceOf(g_depth);
    UpdateSampleScale();
    return fresh;
}
}  // namespace

HLE_STDCALL(Direct3D_CreateDevice, 6) {
    XDK_REQUIRE("d3d", XDK_DATA(D3D_Device), XDK_DATA(D3D_pDevice), XDK_DATA(D3D_DirtyFlags),
                XDK_DATA(D3D_DeferredTextureState), XDK_DATA(D3D_RenderState), XDK_DATA(D3D_Streams),
                XDK_DATA(D3D_SimpleRenderStateEncode));
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Push), XDK_KEY(CDevice_PushThreshold), XDK_KEY(CDevice_BaseVertex),
                XDK_KEY(CDevice_IndexBuffer), XDK_KEY(CDevice_ZScale), XDK_KEY(CDevice_SuperSampleX),
                XDK_KEY(CDevice_SuperSampleY), XDK_KEY(CDevice_Transforms), XDK_KEY(CDevice_Textures),
                XDK_KEY(CDevice_RenderTarget), XDK_KEY(CDevice_Depth), XDK_KEY(CDevice_SwapCount), XDK_KEY(CDevice_Size),
                XDK_KEY(DirtyFlags_Texture), XDK_KEY(TextureState_Stage), XDK_KEY(Stream_Size),
                XDK_KEY(Stream_Stride), XDK_KEY(Stream_Buffer));
    if (XDK_CDevice_FrameSurfaces == xdk::kUnknown)
        XDK_REQUIRE("d3d", XDK_KEY(CDevice_BackBuffer), XDK_KEY(CDevice_FrontBuffer));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t pp = a[4];  // D3DPRESENT_PARAMETERS
    uint32_t w = R32(pp), h = R32(pp + 4), fmt = R32(pp + 8), count = std::max<uint32_t>(R32(pp + 12), 1);
    uint32_t auto_depth = R32(pp + 0x20), depth_fmt = R32(pp + 0x24), interval = R32(pp + 0x30);
    if (!w) w = 640;
    if (!h) h = 480;
    LOGI("d3d: CreateDevice %ux%u format %X buffers %u depth %u format %X interval %X flags %X", w, h, fmt, count,
         auto_depth, depth_fmt, interval, R32(pp + 0x28));
    // 3948 keeps the frame buffer objects in the device: kept across the clear, so the same parameters reuse them
    uint8_t frame_surfaces[3 * 24], auto_depth_surface[24];
    bool inline_surfaces = XDK_CDevice_FrameSurfaces != xdk::kUnknown && g_buffers[1];
    if (inline_surfaces) {
        std::memcpy(frame_surfaces, HostPtr(kDevice + XDK_CDevice_FrameSurfaces), sizeof frame_surfaces);
        if (XDK_CDevice_AutoDepthSurface != xdk::kUnknown)
            std::memcpy(auto_depth_surface, HostPtr(kDevice + XDK_CDevice_AutoDepthSurface), sizeof auto_depth_surface);
    }
    std::memset(HostPtr(kDevice), 0, XDK_CDevice_Size);
    if (inline_surfaces) {
        std::memcpy(HostPtr(kDevice + XDK_CDevice_FrameSurfaces), frame_surfaces, sizeof frame_surfaces);
        if (XDK_CDevice_AutoDepthSurface != xdk::kUnknown)
            std::memcpy(HostPtr(kDevice + XDK_CDevice_AutoDepthSurface), auto_depth_surface, sizeof auto_depth_surface);
    }
    W32(kDevicePtr, kDevice);
    ScratchPush();

    if (!g_backend) {
        g_backend = gpu::CreateBackend();
        if (!g_backend->Init(w, h)) {
            // the failed one goes first: it may hold the window's swap chain
            g_backend.reset();
            g_backend = gpu::CreateFallbackBackend();
            if (g_backend && g_backend->Init(w, h)) {
                LOGR("d3d: the chosen renderer could not start, running on the fallback (Direct3D 11)");
            } else {
                LOGE("d3d: renderer init failed");
                if (!g_backend) g_backend = gpu::CreateBackend();
            }
        }
        if (RenderThread()) g_backend = gpu::CreateThreadedBackend(std::move(g_backend));
    }
    g_state = gpu::State{};
    MakeFrameBuffers(pp);
    // where the library's own CreateDevice keeps the present parameters' interval (3948)
    if (XDK_CDevice_PresentInterval != xdk::kUnknown) SetDev(XDK_CDevice_PresentInterval, interval);
    // 60 Hz, widescreen
    if (XDK_CDevice_DisplayFlags != xdk::kUnknown) SetDev(XDK_CDevice_DisplayFlags, 0x400000 | (app::Widescreen() ? 0x10000 : 0));
#if HAS_D3D_FvfShader
    if (XDK_CDevice_VertexShader != xdk::kUnknown) SetDev(XDK_CDevice_VertexShader, SYM(D3D_FvfShader));
#endif
    if (kVBlank) {
        // notification event, signalled
        SetDev(XDK_CDevice_VBlankEvent, 0x00040000);
        SetDev(XDK_CDevice_VBlankEvent + 4, 1);
        static bool started = false;
        if (!started) StartHostThread("vertical blank", VerticalBlank);
        started = true;
    }
    UpdateViewportConstants();
    ApplyDefaultStates(ctx);
    if (a[5]) W32(a[5], kDevice);
    return 0;
}

// Reset(parameters): the frame buffers for the parameters become the target and are cleared, as the library's own
// Reset does; render states stay. The renderer keeps the size it started with.
HLE_STDCALL(D3DDevice_Reset, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t pp = a[0];
    LOGI("d3d: Reset %ux%u format %X buffers %u depth %u format %X interval %X flags %X", R32(pp), R32(pp + 4),
         R32(pp + 8), R32(pp + 12), R32(pp + 0x20), R32(pp + 0x24), R32(pp + 0x30), R32(pp + 0x28));
    uint32_t old_rt = Dev(kDevRenderTarget), old_zs = Dev(kDevDepth);
    bool fresh = MakeFrameBuffers(pp);
    // the device's references move to the new targets as SetRenderTarget moves them; new surfaces come with theirs
    for (auto [now, old] : {std::pair{Dev(kDevRenderTarget), old_rt}, std::pair{Dev(kDevDepth), old_zs}}) {
        if (now == old) continue;
        if (!fresh) AddRef(ctx, now);
        Release(ctx, old);
    }
    UpdateViewportConstants();
    constexpr uint32_t kClearTarget = 1, kClearZBuffer = 2;
    if (g_backend) g_backend->Clear(g_state, 0, nullptr, kClearTarget | kClearZBuffer, 0, 1.0f, 0);
    return 0;
}

HLE_STDCALL(D3DDevice_Release, 0) { return 1; }

// SetDepthClipPlanes(near, far, flags): the hardware's Z clip planes. The library's own one rewrites device state the
// host never set up. The renderer clips at the viewport's depth range, as the default planes do; other planes are
// not applied yet.
HLE_STDCALL(D3DDevice_SetDepthClipPlanes, 3) {
    LOG_FIRST(4, "d3d: SetDepthClipPlanes %g %g flags %u is not applied", ArgF(a, 0), ArgF(a, 1), a[2]);
    return 0;
}

// synchronisation, the renderer never runs behind

HLE_STDCALL(D3D_BlockOnResource, 1) {
    uint32_t res = a[0];
    if (res && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        uint32_t type = R32(res) & 0x70000;  // 0x40000 texture, 0x50000 surface
        if (type == 0x40000 || type == 0x50000) {
            uint32_t data = mem::VirtualFromPhysical(R32(res + 4));
            // perf diagnostic: who blocks on which surface, once each
            static std::unordered_set<uint32_t> seen;
            if (diag::Perf() && seen.insert(data).second) {
                uint32_t code[4] = {}, n = 0;
                for (uint32_t i = 0; i < 32 && n < 4; i++) {
                    uint32_t v = R32(ctx.esp + i * 4);
                    if (v >= X86CodeBase && v < X86CodeEnd) code[n++] = v;
                }
                LOGI("d3d: BlockOnResource on surface %08X, stack %08X %08X %08X %08X", data, code[0], code[1], code[2], code[3]);
            }
            g_backend->Sync(data, 0);
        } else {
            g_backend->WaitResource(res);
        }
    }
    gpu::MarkDirty(res);
    return 0;
}
// lock(resource, face, level, locked, rect, flags): D3DLOCK_NOOVERWRITE skips the block above, yet a
// title may read what the GPU drew
HLE_WRAP(Lock2DSurface, 6) {
    uint32_t res = a[0];
    if ((a[5] & 0x20) && res && g_backend) {  // D3DLOCK_NOOVERWRITE
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->SyncLate(mem::VirtualFromPhysical(R32(res + 4)));
    }
}
HLE_STDCALL(D3DResource_BlockUntilNotBusy, 1) {
    if (a[0] && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->WaitResource(a[0]);
    }
    gpu::MarkDirty(a[0]);
    return 0;
}
HLE_STDCALL(D3DResource_IsBusy, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t res = a[0];
    if (!res) return 0;
    // set on the device, a surface also through its texture
    uint32_t common = R32(res), parent = (common & 0x70000) == 0x50000 ? R32(res + 0x14) : 0;
    if ((common & 0x780000) || (parent && (R32(parent) & 0x780000))) return 1;  // internal reference count
    return g_backend && g_backend->Busy(res) ? 1 : 0;
}
HLE_STDCALL(D3D_BlockOnTime, 2) { return 0; }
HLE_STDCALL(D3DDevice_BlockOnFence, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->WaitFence(a[0]);
    return 0;
}
HLE_STDCALL(D3DDevice_InsertFence, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3DDevice_IsFencePending, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend && g_backend->FencePending(a[0]) ? 1 : 0;
}
// The library's GetVisibilityTestResult sets a fence when the test's own one is current, so the GPU runs on to the
// report, and titles poll it in a loop without presenting: a fence after a report asks for it to be written. The
// title does not wait for that here: the render thread finishes what it has and writes the report in its own time
// (a title polling in a loop sees it then). Waiting stalled every frame with a report in it: Swap sets a fence too
// (PGR2: about 9 ms, the render thread's backlog, in frames with its sun and light tests).
// XBE_REPORTS_EXACT=1: the title waits, as before
HLE_STDCALL(D3D_SetFence, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    if (g_backend && g_report_asked) {
        static const bool exact = [] {
            const char* e = std::getenv("XBE_REPORTS_EXACT");
            bool on = e && *e == '1';
            if (on) LOGR("d3d: XBE_REPORTS_EXACT=1, a fence after a visibility report waits for the GPU");
            return on;
        }();
        g_report_asked = false;
        if (exact) g_backend->Flush();
        else g_backend->Kick();
    }
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3D_KickOffAndWaitForIdle, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->Flush();
    return 0;
}
HLE_FASTCALL(CDevice_KickOff, 0) { return 0; }

HLE_STDCALL(D3DDevice_MakeSpace, 0) {
    LOG_FIRST(16, "d3d: MakeSpace from %08X, some caller still pushes hardware commands", R32(ctx.esp));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return ScratchPush();
}
HLE_STDCALL(D3D_MakeRequestedSpace, 2) {
    LOG_FIRST(16, "d3d: MakeRequestedSpace from %08X", R32(ctx.esp));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return ScratchPush();
}

HLE_STDCALL(D3DDevice_BlockUntilVerticalBlank, 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    return 0;
}
// (D3DRASTER_STATUS*): InVBlank, ScanLine. The library reads the CRTC's raster register through the GPU register base
// the host device does not have (NightFire's XMV player polls it); the position is the time since the host's last
// vertical blank over a 525 line frame, 480 of them shown, ScanLine 0 within the blank as the library reports it
HLE_STDCALL(D3DDevice_GetRasterStatus, 1) {
    constexpr int64_t kFrameNs = 16666667, kLines = 525, kShown = 480;
    int64_t since = std::chrono::steady_clock::now().time_since_epoch().count() - g_vblank_time.load();
    int64_t line = std::clamp<int64_t>(since, 0, kFrameNs - 1) * kLines / kFrameNs;
    bool blank = line >= kShown;
    W32(a[0], blank ? 1 : 0);
    W32(a[0] + 4, blank ? 0 : uint32_t(line));
    return 0;
}
HLE_STDCALL(D3DDevice_SetFlickerFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_SetSoftDisplayFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_EnableOverlay, 1) { return 0; }
// (surface, source rect, destination rect, colour key enabled, colour key): the video overlay is not shown yet. The
// library's own writes the overlay's GPU registers (PVIDEO, 0x8700 on) through the register base the host device
// does not have (Forza: its movies)
HLE_STDCALL(D3DDevice_UpdateOverlay, 5) {
    static uint32_t calls = 0;
    if (calls++ % 600 == 0)
        LOGI("d3d: UpdateOverlay %u: surface %08X (format %08X, size %08X, data %08X) to %d,%d-%d,%d (not shown)", calls,
             a[0], R32(a[0] + 0xC), R32(a[0] + 0x10), R32(a[0] + 4), a[2] ? int32_t(R32(a[2])) : -1,
             a[2] ? int32_t(R32(a[2] + 4)) : -1, a[2] ? int32_t(R32(a[2] + 8)) : -1, a[2] ? int32_t(R32(a[2] + 12)) : -1);
    return 0;
}
// the front buffer, read back at the guest size, is kept for the next instance after a quick reboot, which may
// fade from it at its first frames
HLE_STDCALL(D3DDevice_PersistDisplay, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t front = g_buffers[0], size = R32(front + 0x10);
    uint32_t pitch = ((size >> 24 & 0xFF) + 1) * 64, height = (size >> 12 & 0xFFF) + 1, bytes = pitch * height;
    uint32_t data = mem::VirtualFromPhysical(R32(front + 4));
    if (g_backend) g_backend->Sync(data, bytes);
    kernel::SavePersistedDisplay(R32(front), R32(front + 0xC), size, HostPtr(data), bytes);
    LOGI("d3d: PersistDisplay, %u bytes", bytes);
    return 0;
}
HLE_STDCALL(D3DDevice_FlushVertexCache, 0) { return 0; }

HLE_STDCALL(D3DDevice_SetGammaRamp, 2) {
    WalkBeforeState();
    uint8_t ramp[768];
    std::memcpy(ramp, HostPtr(a[1]), 768);
    if (g_backend) g_backend->SetGamma(ramp);
    return 0;
}

// targets and presentation

// the device's targets and the renderer's, references moved from the old ones
void SetTargets(X86Context& ctx, uint32_t rt, uint32_t zs) {
    AddRef(ctx, rt);
    uint32_t old_rt = Dev(kDevRenderTarget), old_zs = Dev(kDevDepth);
    SetDev(kDevRenderTarget, rt);
    SetDev(kDevDepth, zs);
    AddRef(ctx, zs);
    Release(ctx, old_rt);
    Release(ctx, old_zs);
    g_state.rt = SurfaceOf(rt);
    g_state.zs = SurfaceOf(zs);
    UpdateSampleScale();
}

HLE_STDCALL(D3DDevice_SetRenderTarget, 2) {
    WalkBeforeState();
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    SetTargets(ctx, a[0] ? a[0] : Dev(kDevRenderTarget), a[1]);
    // SetRenderTarget resets the viewport to the whole target
    uint32_t w, h;
    gpu::SurfaceSize(g_state.rt, w, h);
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    UpdateViewportConstants();
    return 0;
}

// the same without the viewport reset; flags (D3DSRTF_*) only steer the library's own push buffer writes. The
// library's version wrote the surfaces to the hardware directly: draws went on into the previous target (a title
// drawing a cube map's six faces through it filled face 0 six times).
HLE_STDCALL(D3DDevice_SetRenderTargetFast, 3) {
    WalkBeforeState();
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    SetTargets(ctx, a[0] ? a[0] : Dev(kDevRenderTarget), a[1]);
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_GetBackBuffer2, 1) {
    int32_t i = int32_t(a[0]);
    uint32_t s = i < 0 ? g_buffers[0] : g_buffers[std::min<uint32_t>(uint32_t(i) + 1, g_buffer_count - 1)];
    W32(s, R32(s) + 1);
    return s;
}

// 3948's forms hand the surface out through a pointer and return an HRESULT
HLE_STDCALL(D3DDevice_GetBackBuffer, 3) {
    int32_t i = int32_t(a[0]);
    uint32_t s = i < 0 ? g_buffers[0] : g_buffers[std::min<uint32_t>(uint32_t(i) + 1, g_buffer_count - 1)];
    W32(s, R32(s) + 1);
    if (a[2]) W32(a[2], s);
    return 0;
}
HLE_STDCALL(D3DDevice_GetDepthStencilSurface, 1) {
    uint32_t s = Dev(kDevDepth);
    if (a[0]) W32(a[0], s);
    if (!s) return 0x88760866u;  // D3DERR_NOTFOUND
    W32(s, R32(s) + 1);
    return 0;
}

HLE_STDCALL(D3DDevice_GetRenderTarget2, 0) {
    uint32_t s = Dev(kDevRenderTarget);
    if (s) W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_GetDepthStencilSurface2, 0) {
    uint32_t s = Dev(kDevDepth);
    if (s) W32(s, R32(s) + 1);
    return s;
}

namespace {
std::atomic<uint32_t> g_presents{0};

// vertical blanks a frame the title asked for (D3DPRESENT_PARAMETERS or SetPresentationInterval, kept in a library
// global the swap reads): DEFAULT 0 and ONE 1 wait for one, TWO 2 to FOUR 4 for that many, IMMEDIATE 0x80000000
// none, and the _OR_IMMEDIATE values 0x80000001 to 0x80000003 as many unless the frame is late. 1 when unknown.
uint32_t PresentVblanks() {
    if (!HAS_D3D_PresentInterval && XDK_CDevice_PresentInterval == xdk::kUnknown) return 1;
    uint32_t v = HAS_D3D_PresentInterval ? R32(SYM(D3D_PresentInterval)) : Dev(XDK_CDevice_PresentInterval);
    static uint32_t last = ~0u;
    if (v != last) LOGR("d3d: present interval %08X", last = v);
    if (v == 0x80000000) return 0;
    v &= 0x7FFFFFFF;
    return v >= 1 && v <= 4 ? v : 1;
}
}  // namespace

// Swap(flags): D3DSWAP_COPY 1 and D3DSWAP_BYPASSCOPY 2 without D3DSWAP_FINISH 4 open the copy to the front buffer
// (the target becomes the front, texture 0 the back buffer, as the library sets them); FINISH shows the frame and
// gives the back buffer and depth back as the target. 0 is COPY | FINISH. The count moves on once a frame.
namespace {
uint32_t SwapFrame(X86Context& ctx, uint32_t swap_flags) {
    constexpr uint32_t kCopy = 1, kBypassCopy = 2, kFinish = 4;
    uint32_t flags = swap_flags ? swap_flags : kCopy | kFinish;
    if (!(flags & kFinish)) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        if (!(flags & (kCopy | kBypassCopy)) || !g_copy_target) return Dev(kDevSwapCount);
        Walk(Dev(kDevPush));
        if (!g_copy_open) SetDev(kDevSwapCount, Dev(kDevSwapCount) + 1);
        g_copy_open = true;
        SetDev(kDevRenderTarget, g_copy_target);
        SetDev(kDevDepth, 0);
        g_state.rt = SurfaceOf(g_copy_target);
        g_state.zs = SurfaceOf(0);
        UpdateSampleScale();
        // in the device's slots too, with their references: the title may read them back (Forza 0x1DB3C0 takes
        // texture 0 with GetTexture2 and releases it)
        for (uint32_t i = 0; i < 4; i++) {
            uint32_t tex = i ? 0 : g_buffers[1], old = Dev(kDevTextures + i * 4);
            g_state.textures[i] = tex;
            if (tex == old) continue;
            AddRef(ctx, tex);
            SetDev(kDevTextures + i * 4, tex);
            Release(ctx, old);
        }
        W32(kDirtyFlags, R32(kDirtyFlags) | XDK_DirtyFlags_Texture);
        uint32_t w, h;
        gpu::SurfaceSize(g_state.rt, w, h);
        g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
        UpdateViewportConstants();
        return Dev(kDevSwapCount);
    }
    app::SplashFirstFrame();
    // handed over from the previous instance: the window opens on the second frame, the first picture with
    // anything drawn over it right after
    if (app::HandingOver() && g_presents >= 2) {
        {
            std::lock_guard<std::recursive_mutex> lock(g_lock);
            if (g_backend) g_backend->Flush();
        }
        app::FinishHandover();
    }
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_presents++;
    // the kept display is shown until the title presents its own
    kernel::SetSavedDataAddress(0);
    // the ramp an inlined SetGammaRamp left for the vertical blank that shows this swap
    if (g_backend && XDK_CDevice_GammaPending != xdk::kUnknown && XDK_CDevice_GammaRamps != xdk::kUnknown) {
        uint32_t i = Dev(kDevSwapCount) & 1;
        if (Dev(XDK_CDevice_GammaPending + i * 4) == 1) {
            SetDev(XDK_CDevice_GammaPending + i * 4, 0);
            g_backend->SetGamma(static_cast<const uint8_t*>(HostPtr(kDevice + XDK_CDevice_GammaRamps + i * 0x300)));
        }
    }
    Walk(Dev(kDevPush));  // reports asked for at the end of the frame
    if (g_backend) g_backend->Present(SurfaceOf(g_buffers[1]));
    achievements::Frame();
    app::Pace(PresentVblanks());
    // like the XDK, the surfaces stay put and their memory rotates: titles keep back buffer pointers and copy its data
    uint32_t last = g_buffers[g_buffer_count - 1], data = R32(last + 4), lk = R32(last + 8);
    for (uint32_t i = g_buffer_count - 1; i > 0; i--) {
        W32(g_buffers[i] + 4, R32(g_buffers[i - 1] + 4));
        W32(g_buffers[i] + 8, R32(g_buffers[i - 1] + 8));
    }
    W32(g_buffers[0] + 4, data);
    W32(g_buffers[0] + 8, lk);
    if (g_copy_open) {
        // the copy is done: the back buffer and the auto depth buffer are the target again, as after the library's
        g_copy_open = false;
        for (uint32_t i = 0; i < 4; i++) g_state.textures[i] = Dev(kDevTextures + i * 4);  // what the title bound
        W32(kDirtyFlags, R32(kDirtyFlags) | XDK_DirtyFlags_Texture);
        SetDev(kDevRenderTarget, g_buffers[1]);
        SetDev(kDevDepth, g_depth);
        g_state.zs = SurfaceOf(g_depth);
        uint32_t w, h;
        gpu::SurfaceSize(SurfaceOf(g_buffers[1]), w, h);
        g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
        UpdateViewportConstants();
    } else {
        SetDev(kDevSwapCount, Dev(kDevSwapCount) + 1);
    }
    g_state.rt = SurfaceOf(Dev(kDevRenderTarget));
    UpdateSampleScale();
    return Dev(kDevSwapCount);
}
}  // namespace

HLE_STDCALL(D3DDevice_Swap, 1) { return SwapFrame(ctx, a[0]); }
// 3948: Present(source rect, dest rect, window, dirty region) is the swap, the rects unused on the console
HLE_STDCALL(D3DDevice_Present, 4) {
    SwapFrame(ctx, 0);
    return 0;
}

HLE_STDCALL(D3DDevice_Clear, 6) {
    WalkBeforeState();
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    static const bool log_clears = std::getenv("XBE_CLEAR_LOG") != nullptr;  // each clear: target, flags, colour
    if (log_clears)
        LOG_FIRST(100000, "d3d: Clear frame %u target data %08X depth data %08X rects %u flags %X colour %08X", g_presents.load(),
                  Dev(kDevRenderTarget) ? R32(Dev(kDevRenderTarget) + 4) : 0, Dev(kDevDepth) ? R32(Dev(kDevDepth) + 4) : 0,
                  a[0], a[2], a[3]);
    const uint8_t* rects = a[0] && a[1] ? static_cast<const uint8_t*>(HostPtr(a[1])) : nullptr;
    if (g_backend) g_backend->Clear(g_state, a[0], rects, a[2], a[3], ArgF(a, 4), a[5]);
    return 0;
}

HLE_STDCALL(D3DDevice_CopyRects, 5) {
    WalkBeforeState();
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->Copy(SurfaceOf(a[0]), SurfaceOf(a[3]), a[2], a[1], a[4]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetViewport, 1) {
    WalkBeforeState();
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t v = a[0];
    g_state.viewport = {R32(v), R32(v + 4), R32(v + 8), R32(v + 12), GF(v + 16), GF(v + 20)};
    g_state.scissor_count = 0;
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_SetScissors, 3) {
    WalkBeforeState();
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.scissor_count = std::min<uint32_t>(a[0], 8);
    for (uint32_t i = 0; i < g_state.scissor_count * 4; i++) g_state.scissors[i] = R32(a[2] + i * 4);
    return 0;
}

// (x, y): the part of the back buffer the title draws into, kept where the library keeps it; the library's own then
// rebuilds the viewport from render target fields the host device does not fill (Forza 0x3BCD40). Drawing ignores it
HLE_STDCALL(D3DDevice_SetBackBufferScale, 2) {
    float x = ArgF(a, 0), y = ArgF(a, 1);
    if (x != 1.0f || y != 1.0f) LOG_FIRST(8, "d3d: SetBackBufferScale %.3f %.3f (drawn at full size)", x, y);
    if (XDK_CDevice_BackBufferScale != xdk::kUnknown) {
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale), &x, 4);
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale + 4), &y, 4);
    }
    return 0;
}

HLE_STDCALL(D3DDevice_SetScreenSpaceOffset, 2) {
    WalkBeforeState();
    g_state.ss_offset_x = ArgF(a, 0);
    g_state.ss_offset_y = ArgF(a, 1);
    UpdateViewportConstants();
    return 0;
}

// render states

HLE_FASTCALL(D3DDevice_SetRenderState_Simple, 0) {
    WalkBeforeState();
    uint32_t method = a[0] & 0x1FFC, value = a[1];
    g_state.simple[method >> 2] = value;
    return 0;
}

HLE_STDCALL(D3DDevice_SetRenderStateNotInline, 2) {
    WalkBeforeState();
    uint32_t rs = a[0], v = a[1];  // the title's numbering
    if (rs < xdk::states::kNumbering.deferred_first) {
        uint32_t method = R32(kSimpleEncode + rs * 4) & 0x1FFC;
        g_state.simple[method >> 2] = v;
        W32(kRenderState + rs * 4, v);  // titles read their states back from the library's table
    } else if (uint32_t c = xdk::states::RsFromTitle(rs); c != xdk::states::kNone) {
        SetComplex(c, v);
    } else if (rs < xdk::states::kNumbering.count) {
        W32(kRenderState + rs * 4, v);  // a state 5659 lacks (3948's MultiSampleType): kept for the title only
    }
    return 0;
}

#define COMPLEX_STATE(name, rs)                       \
    HLE_STDCALL(D3DDevice_SetRenderState_##name, 1) { \
        SetComplex(rs, a[0]);                         \
        return 0;                                     \
    }
COMPLEX_STATE(PSTextureModes, gpu::kRsPsTextureModes)
COMPLEX_STATE(VertexBlend, gpu::kRsVertexBlend)
COMPLEX_STATE(FogColor, gpu::kRsFogColor)
COMPLEX_STATE(FillMode, gpu::kRsFillMode)
COMPLEX_STATE(BackFillMode, gpu::kRsBackFillMode)
COMPLEX_STATE(TwoSidedLighting, gpu::kRsTwoSidedLighting)
COMPLEX_STATE(NormalizeNormals, gpu::kRsNormalizeNormals)
COMPLEX_STATE(ZEnable, gpu::kRsZEnable)
COMPLEX_STATE(StencilEnable, gpu::kRsStencilEnable)
COMPLEX_STATE(StencilFail, gpu::kRsStencilFail)
COMPLEX_STATE(FrontFace, gpu::kRsFrontFace)
COMPLEX_STATE(CullMode, gpu::kRsCullMode)
COMPLEX_STATE(TextureFactor, gpu::kRsTextureFactor)
COMPLEX_STATE(LogicOp, gpu::kRsLogicOp)
COMPLEX_STATE(EdgeAntiAlias, gpu::kRsEdgeAntiAlias)
COMPLEX_STATE(MultiSampleAntiAlias, gpu::kRsMultiSampleAntiAlias)
COMPLEX_STATE(MultiSampleMask, gpu::kRsMultiSampleMask)
// the multisample modes change where draws land (State::aa_x): draws pushed before them are read under the old mode
// first (EoN's particles batched inside its 4X window were drawn after it at half their place: a second muzzle flash)
#define SAMPLE_STATE(name, rs)                        \
    HLE_STDCALL(D3DDevice_SetRenderState_##name, 1) { \
        WalkBeforeState();                            \
        SetComplex(rs, a[0]);                         \
        return 0;                                     \
    }
SAMPLE_STATE(MultiSampleMode, gpu::kRsMultiSampleMode)
SAMPLE_STATE(MultiSampleRenderTargetMode, gpu::kRsMultiSampleRenderTargetMode)
COMPLEX_STATE(ShadowFunc, gpu::kRsShadowFunc)
COMPLEX_STATE(LineWidth, gpu::kRsLineWidth)
COMPLEX_STATE(SampleAlpha, gpu::kRsSampleAlpha)
COMPLEX_STATE(Dxt1NoiseEnable, gpu::kRsDxt1NoiseEnable)
COMPLEX_STATE(YuvEnable, gpu::kRsYuvEnable)
COMPLEX_STATE(OcclusionCullEnable, gpu::kRsOcclusionCullEnable)
COMPLEX_STATE(StencilCullEnable, gpu::kRsStencilCullEnable)
COMPLEX_STATE(RopZCmpAlwaysRead, gpu::kRsRopZCmpAlwaysRead)
COMPLEX_STATE(RopZRead, gpu::kRsRopZRead)
COMPLEX_STATE(DoNotCullUncompressed, gpu::kRsDoNotCullUncompressed)

HLE_STDCALL(D3DDevice_SetTextureState_TexCoordIndex, 2) {
    WalkBeforeState();
    StoreTss(a[0] & 3, gpu::kTssTexCoordIndex, a[1]);
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BumpEnv, 3) {
    WalkBeforeState();
    uint32_t t = xdk::states::TssFromTitle(a[1]);
    if (t >= gpu::kTssBumpEnvMat00 && t <= gpu::kTssBumpEnvLOffset) g_state.tss[a[0] & 3][t] = a[2];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BorderColor, 2) {
    WalkBeforeState();
    g_state.tss[a[0] & 3][gpu::kTssBorderColor] = a[1];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_ColorKeyColor, 2) {
    WalkBeforeState();
    g_state.tss[a[0] & 3][gpu::kTssColorKeyColor] = a[1];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureStageStateNotInline, 3) {
    WalkBeforeState();
    uint32_t stage = a[0] & 3, type = xdk::states::TssFromTitle(a[1]), v = a[2];
    if (type != xdk::states::kNone) StoreTss(stage, type, v);
    return 0;
}

// the XDK's transform types: view 0, projection 1, texture 2-5, world 6-9; the renderer keeps Direct3D 8's numbers
// (view 2, projection 3, texture 16, world 256). The device copy is what GetTransform reads.
HLE_STDCALL(D3DDevice_SetTransform, 2) {
    WalkBeforeState();
    uint32_t t = a[0];
    if (t >= 10) return 0;
    std::memcpy(HostPtr(kDevice + kDevTransforms + t * 64), HostPtr(a[1]), 64);
    uint32_t pc = t < 2 ? t + 2 : t < 6 ? t + 14 : t + 250;
    std::memcpy(g_state.transforms[pc].data(), HostPtr(a[1]), 64);
    if (t == 1) {  // the far w of the projection, as the library's own SetTransform finds it
        const float* m = g_state.transforms[pc].data();
        float wfar = 1;
        if (m[10] != m[11] && m[10] != 0) wfar = (m[15] - m[14]) / (m[10] - m[11]) * m[11] + m[15];
        g_state.inv_wfar = wfar != 0 && std::isfinite(1 / wfar) ? 1 / wfar : 1;
        LOG_FIRST(4, "d3d: projection far w %g", wfar);
    }
    return 0;
}

HLE_STDCALL(D3DDevice_SetMaterial, 1) {
    WalkBeforeState();
    std::memcpy(g_state.material.data(), HostPtr(a[0]), 17 * 4);
    return 0;
}

// the XDK keeps its own light lists, the renderer gets a copy
HLE_WRAP(D3DDevice_SetLight, 2) {
    uint32_t i = a[0], l = a[1];
    if (i < 8 && l) {
        WalkBeforeState();
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        std::memcpy(g_state.lights[i].data(), HostPtr(l), 26 * 4);
    }
}
HLE_WRAP(D3DDevice_LightEnable, 2) {
    uint32_t i = a[0], on = a[1];
    if (i < 8) {
        WalkBeforeState();
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_state.light_enable = on ? g_state.light_enable | 1u << i : g_state.light_enable & ~(1u << i);
    }
}

// resources in use

HLE_STDCALL(D3DDevice_SetTexture, 2) {
    WalkBeforeState();
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t stage = a[0] & 3, tex = a[1];
    uint32_t old = Dev(kDevTextures + stage * 4);
    AddRef(ctx, tex);
    SetDev(kDevTextures + stage * 4, tex);
    Release(ctx, old);
    g_state.textures[stage] = tex;
    W32(kDirtyFlags, R32(kDirtyFlags) | XDK_DirtyFlags_Texture);
    uint32_t pal = XDK_CDevice_Palettes == xdk::kUnknown ? 0 : Dev(kDevPalettes + stage * 4);
    if (tex && app::kGame.texture_bound && app::kGame.texture_bound(tex, pal)) gpu::MarkDirty(tex);
    return 0;
}

// SetPalette(stage, palette): the colours a P8 texture of the stage looks up
HLE_STDCALL(D3DDevice_SetPalette, 2) {
    WalkBeforeState();
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Palettes));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t stage = a[0] & 3, pal = a[1];
    uint32_t old = Dev(kDevPalettes + stage * 4);
    AddRef(ctx, pal);
    SetDev(kDevPalettes + stage * 4, pal);
    Release(ctx, old);
    g_state.palettes[stage] = pal;
    LOG_FIRST(4, "d3d: SetPalette stage %u palette %08X", stage, pal);
    uint32_t tex = g_state.textures[stage];
    if (tex && pal && app::kGame.texture_bound && app::kGame.texture_bound(tex, pal)) gpu::MarkDirty(tex);
    return 0;
}

// SwitchTexture(method, data, format), fastcall: a swizzled texture of the bound one's shape replaces it. It is
// bound through a header of the stage's own holding the bound texture's fields with the new data and format.
HLE_FASTCALL(D3DDevice_SwitchTexture, 1) {
    WalkBeforeState();
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    static uint32_t headers[4];
    // texture methods from 0x1B00, 0x40 per stage
    uint32_t stage = (((a[0] & 0x1FFC) - 0x1B00) >> 6) & 3, cur = g_state.textures[stage];
    uint32_t& h = headers[stage];
    if (!h) h = mem::AllocSystem(20);
    if (cur && cur != h) std::memcpy(HostPtr(h), HostPtr(cur), 20);
    W32(h + 4, a[1]);
    W32(h + 12, a[2]);
    g_state.textures[stage] = h;
    // a title watching what it binds sees this as a bind too (EoN's title art is switched in)
    if (app::kGame.texture_bound && app::kGame.texture_bound(h, 0)) gpu::MarkDirty(h);
    return 0;
}

HLE_STDCALL(D3DDevice_SetStreamSource, 3) {
    WalkBeforeState();
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t n = a[0] & 15, vb = a[1], stride = a[2];
    uint32_t old = R32(kStreams + n * kStreamSize + XDK_Stream_Buffer);
    AddRef(ctx, vb);
    W32(kStreams + n * kStreamSize + XDK_Stream_Stride, stride);
    W32(kStreams + n * kStreamSize + XDK_Stream_Buffer, vb);
    Release(ctx, old);
    g_state.streams[n] = {vb, stride};
    return 0;
}

HLE_STDCALL(D3DDevice_SetIndices, 2) {
    WalkBeforeState();
    XDK_REQUIRE("d3d", XDK_DATA(D3D_IndexData));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t ib = a[0];
    uint32_t old = Dev(kDevIndexBuffer);
    AddRef(ctx, ib);
    W32(kIndexData, ib ? R32(ib + 4) : 0);
    SetDev(kDevIndexBuffer, ib);
    SetDev(kDevBaseVertex, a[1]);
    Release(ctx, old);
    g_state.index_buffer = ib;
    g_state.index_data = ib ? R32(ib + 4) : 0;
    g_state.base_vertex = a[1];
    return 0;
}

// shaders

HLE_STDCALL(D3DDevice_CreateVertexShader, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t decl = a[0], func = a[1], out = a[2];
    auto vs = std::make_shared<gpu::VertexShader>();
    if (decl) {
        for (uint32_t i = 0; i < 1024; i++) {
            uint32_t t = R32(decl + i * 4);
            vs->decl.push_back(t);
            if (t == 0xFFFFFFFF) break;
        }
    }
    if (func) {
        uint32_t hdr = R32(func);
        vs->program_type = hdr & 0xFFFF;
        uint32_t n = hdr >> 16;
        for (uint32_t i = 0; i < n * 4; i++) vs->program.push_back(R32(func + 4 + i * 4));
    }
    gpu::ParseDeclaration(*vs);
    uint32_t rec = mem::AllocSystem(16, 16);
    vs->handle = rec | 1;
    g_vs[vs->handle] = vs;
    W32(out, vs->handle);
    LOG_FIRST(64, "d3d: vertex shader %08X, %zu decl tokens, %zu instructions, type %X", vs->handle, vs->decl.size(),
              vs->program.size() / 4, vs->program_type);
    return 0;
}

HLE_STDCALL(D3DDevice_DeleteVertexShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_vs.find(a[0]);
    if (it != g_vs.end()) {
        mem::FreeSystem(a[0] & ~1u);
        g_vs.erase(it);
    }
    return 0;
}

// (handle, data, size in and out): the program as it was given, the size alone without data or room
HLE_STDCALL(D3DDevice_GetVertexShaderFunction, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_vs.find(a[0]);
    uint32_t words = it == g_vs.end() ? 0 : uint32_t(it->second->program.size());
    uint32_t need = words ? 4 + words * 4 : 0, room = R32(a[2]);
    W32(a[2], need);
    if (need > room) return a[1] ? 0x88760867 : 0;  // D3DERR_MOREDATA
    if (!a[1] || !need) return 0;
    W32(a[1], it->second->program_type | words / 4 << 16);
    for (uint32_t i = 0; i < words; i++) W32(a[1] + 4 + i * 4, it->second->program[i]);
    return 0;
}

// XBE_VSC: writes to the watched constants, with the caller (ret 0: a raw push buffer write)
void WatchConstants(const char* how, uint32_t first, uint32_t count, uint32_t ret) {
    for (uint32_t r = first; r < first + count && r < 192; r++) {
        if (!diag::WatchedConstant(int(r) - 96)) continue;
        const auto& v = g_state.vsc[r];
        // each distinct register, value and caller once, the first 500; reported in release builds too
        static std::unordered_set<uint64_t> seen;
        uint64_t key = gpu::Hash(v.data(), sizeof(float) * 4, uint64_t(r) << 32 | ret);
        if (seen.size() < 500 && seen.insert(key).second)
            LOGR("d3d: c%d = %g %g %g %g by %s from %08X", int(r) - 96, v[0], v[1], v[2], v[3], how, ret);
    }
}

// SetVertexShaderInput(handle, count, inputs): until called with handle 0, draws read the attributes that vertex
// shader's declaration lays out (stream, offset, type) from count D3DSTREAM_INPUTs (vertex buffer, stride, offset) in
// place of SetStreamSource's streams; the library pushes the vertex array addresses and formats itself (0x1C0560 in
// 5659, dirty bit 0x40000000 makes the draws skip their own). SetVertexShaderInputDirect passes the layout as 16
// D3DVERTEXATTRIBUTEFORMATs (stream, offset, format, 16 bytes each) instead of a handle.
namespace {
void SetVertexInput(const std::array<gpu::VertexShader::Input, 16>& attrs, uint32_t count, uint32_t inputs) {
    std::array<gpu::State::Stream, 16> streams{};
    std::array<gpu::VertexShader::Input, 16> layout{};
    for (size_t r = 0; r < 16; r++) {
        const auto& in = attrs[r];
        constexpr uint32_t kTypeNone = 0x02;
        if (in.stream < 0 || !in.type || (in.type & 0xFF) == kTypeNone || uint32_t(in.stream) >= count || !inputs) continue;
        uint32_t si = inputs + uint32_t(in.stream) * 12;
        streams[size_t(in.stream)] = {R32(si), R32(si + 4)};
        layout[r] = {in.stream, in.offset + R32(si + 8), in.type & 0xFF};
    }
    g_state.input_streams = streams;
    g_state.input_layout = layout;
    g_state.input_active = true;
}
}  // namespace

HLE_STDCALL(D3DDevice_SetVertexShaderInput, 3) {
    WalkBeforeState();
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t h = a[0];
    if (!h) {
        g_state.input_active = false;
        return 0;
    }
    auto it = g_vs.find(h);
    if (it == g_vs.end() || !it->second) {
        LOG_FIRST(8, "d3d: SetVertexShaderInput with unknown handle %08X", h);
        g_state.input_active = false;
        return 0;
    }
    SetVertexInput(it->second->inputs, a[1], a[2]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetVertexShaderInputDirect, 3) {
    WalkBeforeState();
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t p = a[0];
    if (!p) {
        g_state.input_active = false;
        return 0;
    }
    std::array<gpu::VertexShader::Input, 16> attrs{};
    for (uint32_t r = 0; r < 16; r++)
        attrs[r] = {int(R32(p + r * 16)), R32(p + r * 16 + 4), R32(p + r * 16 + 8) & 0xFF};
    SetVertexInput(attrs, a[1], a[2]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetVertexShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // the declaration's constants come after anything pushed before
    uint32_t h = a[0];
    g_state.vs_handle = h;
    g_state.vs = nullptr;
    g_program_slot = -1;
    if (h & 1) {
        auto it = g_vs.find(h);
        if (it != g_vs.end()) {
            g_state.vs = it->second;
            // constants declared with the shader load when it is selected
            for (auto& [reg, v] : it->second->constants) {
                if (reg < 192) g_state.vsc[reg] = v;
                WatchConstants("SetVertexShader declaration", reg, 1, R32(ctx.esp));
            }
        } else {
            LOG_FIRST(8, "d3d: SetVertexShader with unknown handle %08X", h);
        }
    }
    return 0;
}

// State set by these functions lands at once, state the title pushed as raw commands when Walk reads them: every
// setter reads what was pushed before it first, so both apply in the title's order as on the console (a title
// loading a matrix through the push buffer, then its object's through SetVertexShaderConstantNotInlineFast, drew
// the object with the first; quads pushed into a shadow texture went to the back buffer when SetRenderTarget
// switched back before Walk read them)
void WalkPending() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
}
// the same for the other setters; XBE_STATE_WALK=0 turns it off to compare
void WalkBeforeState() {
    static const bool on = [] {
        const char* e = std::getenv("XBE_STATE_WALK");
        bool v = !(e && *e == '0');
        if (!v) LOGR("d3d: XBE_STATE_WALK=0, state setters do not read pending pushed commands first");
        return v;
    }();
    if (on) WalkPending();
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1Fast, 0) {
    WalkPending();
    uint32_t reg = a[0];
    if (reg < 192) std::memcpy(g_state.vsc[reg].data(), HostPtr(a[1]), 16);
    WatchConstants("SetVertexShaderConstant1Fast", reg, 1, R32(ctx.esp));
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1, 0) {
    WalkPending();
    uint32_t reg = a[0];
    if (reg < 192) std::memcpy(g_state.vsc[reg].data(), HostPtr(a[1]), 16);
    WatchConstants("SetVertexShaderConstant1", reg, 1, R32(ctx.esp));
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant4, 0) {
    WalkPending();
    uint32_t reg = a[0], src = a[1];
    for (uint32_t i = 0; i < 4 && reg + i < 192; i++) std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
    WatchConstants("SetVertexShaderConstant4", reg, 4, R32(ctx.esp));
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstantNotInlineFast, 1) {
    WalkPending();
    uint32_t reg = a[0], src = a[1], dwords = a[2];
    for (uint32_t i = 0; i < dwords / 4 && reg + i < 192; i++) std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
    WatchConstants("SetVertexShaderConstantNotInlineFast", reg, dwords / 4, R32(ctx.esp));
    return 0;
}

// 3948: SetVertexShaderConstant(register -96 to 95, data, count of vectors); the hardware slot is register + 96
HLE_STDCALL(D3DDevice_SetVertexShaderConstant, 3) {
    WalkPending();
    int32_t reg = int32_t(a[0]) + 96;
    uint32_t count = std::min<uint32_t>(a[2], 192);  // a wild count would loop for minutes
    for (uint32_t i = 0; i < count; i++)
        if (reg + int32_t(i) >= 0 && reg + i < 192) std::memcpy(g_state.vsc[reg + i].data(), HostPtr(a[1] + i * 16), 16);
    WatchConstants("SetVertexShaderConstant", uint32_t(reg), a[2], R32(ctx.esp));
    return 0;
}

// (handle, slot): the shader's program into program memory at the slot
HLE_STDCALL(D3DDevice_LoadVertexShader, 2) {
    WalkBeforeState();
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_vs.find(a[0]);
    if (it == g_vs.end()) {
        LOG_FIRST(4, "d3d: LoadVertexShader with unknown handle %08X", a[0]);
        return 0;
    }
    const auto& prog = it->second->program;
    for (uint32_t i = 0, at = std::min(a[1], kProgramSlots) * 4; i < prog.size() && at < kProgramSlots * 4; i++)
        g_program[at++] = prog[i];
    return 0;
}

namespace {
// the shader to draw with after a select: the declaration from the handle (or the inputs given), the program from
// program memory at the slot up to the instruction marked last (bit 0 of its fourth dword)
void SelectProgram(std::shared_ptr<gpu::VertexShader> vs, uint32_t handle, uint32_t slot) {
    vs->hash = 0;
    vs->program.clear();
    vs->program_type = 0x2078;
    for (uint32_t i = std::min(slot, kProgramSlots - 1); i < kProgramSlots; i++) {
        vs->program.insert(vs->program.end(), &g_program[i * 4], &g_program[i * 4] + 4);
        if (g_program[i * 4 + 3] & 1) break;
    }
    vs->handle = handle | 1;
    // the renderers (and EndPrimitive) tell shaders apart by pointer, and titles select hundreds of times a frame
    // (007: Everything or Nothing): one object per declaration and program, or every select is a new shader
    static std::unordered_map<uint64_t, std::shared_ptr<gpu::VertexShader>> made;
    uint64_t key = gpu::Hash(vs->program.data(), vs->program.size() * 4,
                             gpu::Hash(vs->inputs.data(), sizeof(vs->inputs), vs->handle));
    key = gpu::Hash(vs->stream_stride.data(), sizeof(vs->stream_stride), gpu::Hash(vs->decl.data(), vs->decl.size() * 4, key));
    auto& kept = made[key];
    if (!kept || std::memcmp(kept->inputs.data(), vs->inputs.data(), sizeof(vs->inputs)) || kept->program != vs->program ||
        kept->decl != vs->decl || kept->stream_stride != vs->stream_stride || kept->constants != vs->constants)
        kept = vs;
    g_state.vs = kept;
    g_state.vs_handle = handle | 1;
    g_program_slot = int32_t(std::min(slot, kProgramSlots - 1));
    g_program_written = false;
}
}  // namespace

// the selected program again from program memory, once pushed instructions changed it (007: Everything or Nothing
// selects slot 0 and then loads the frame's next program there); the render thread tells shaders apart by
// pointer: SelectProgram keeps one object per declaration and program
void RefreshSelectedProgram() {
    if (!g_program_written) return;
    g_program_written = false;
    if (g_program_slot < 0 || !g_state.vs) return;
    SelectProgram(std::make_shared<gpu::VertexShader>(*g_state.vs), g_state.vs_handle & ~1u, uint32_t(g_program_slot));
}

// (handle, slot): the handle's declaration with the program loaded at the slot (Forza's UI loads its programs
// with LoadVertexShaderProgram, pushed, and selects them through SelectVertexShaderDirect)
HLE_STDCALL(D3DDevice_SelectVertexShader, 2) {
    WalkBeforeState();  // the program loads pushed before it
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto vs = std::make_shared<gpu::VertexShader>();
    if (auto it = g_vs.find(a[0]); it != g_vs.end()) *vs = *it->second;
    SelectProgram(vs, a[0], a[1]);
    return 0;
}

// (inputs, slot): 16 inputs of (stream, offset, type) as SetVertexShaderInputDirect takes them, the program at the
// slot; the library's own copies the inputs into its own shader object and selects that (Forza 0x3BB080)
HLE_STDCALL(D3DDevice_SelectVertexShaderDirect, 2) {
    WalkBeforeState();
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto vs = std::make_shared<gpu::VertexShader>();
    uint32_t p = a[0];
    if (p)
        for (uint32_t r = 0; r < 16; r++) {
            uint32_t type = R32(p + r * 16 + 8) & 0xFF;
            if (type && type != 0x02) vs->inputs[r] = {int(R32(p + r * 16)), R32(p + r * 16 + 4), type};
        }
    SelectProgram(vs, p ? p : 0x10, a[1]);
    return 0;
}
HLE_STDCALL(D3DDevice_SetShaderConstantMode, 1) {
    WalkBeforeState();
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_own_constants = a[0] & 0x10;  // D3DSCM_NORESERVEDCONSTANTS
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_CreatePixelShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto ps = std::make_shared<gpu::PixelShader>();
    std::memcpy(ps->def, HostPtr(a[0]), sizeof(ps->def));
    uint32_t rec = mem::AllocSystem(16, 16);
    ps->handle = rec;
    g_ps[rec] = ps;
    W32(a[1], rec);
    return 0;
}

HLE_STDCALL(D3DDevice_DeletePixelShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_ps.erase(a[0])) mem::FreeSystem(a[0]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShader, 1) {
    WalkBeforeState();
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.ps = nullptr;
    if (!a[0]) return 0;
    auto it = g_ps.find(a[0]);
    if (it != g_ps.end()) {
        g_state.ps = it->second;
    } else if (XDK_PixelShader_Definition != xdk::kUnknown) {
        // made by library code the title inlined: the object points at its definition, handles are reused
        static std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> made;
        const void* def = HostPtr(R32(a[0] + XDK_PixelShader_Definition));
        auto& ps = made[a[0]];
        if (!ps || std::memcmp(ps->def, def, sizeof(ps->def))) {
            ps = std::make_shared<gpu::PixelShader>();
            ps->handle = a[0];
            std::memcpy(ps->def, def, sizeof(ps->def));
        }
        g_state.ps = ps;
    } else {
        LOG_FIRST(8, "d3d: SetPixelShader with unknown handle %08X", a[0]);
        return 0;
    }
    // the definition loads the combiner state like the XDK pushes it
    const uint32_t* d = g_state.ps->def;
    auto put = [&](uint32_t method, uint32_t v) { g_state.simple[method >> 2] = v; };
    for (int i = 0; i < 8; i++) {
        put(gpu::kMPsAlphaInputs + i * 4, d[0 + i]);
        put(gpu::kMPsConstant0 + i * 4, d[10 + i]);
        put(gpu::kMPsConstant1 + i * 4, d[18 + i]);
        put(gpu::kMPsAlphaOutputs + i * 4, d[26 + i]);
        put(gpu::kMPsRgbInputs + i * 4, d[34 + i]);
        put(gpu::kMPsRgbOutputs + i * 4, d[45 + i]);
    }
    put(gpu::kMPsFinalAbcd, d[8]);
    put(gpu::kMPsFinalEfgh, d[9]);
    put(gpu::kMPsCompareMode, d[42]);
    put(gpu::kMPsFinalConstant0, d[43]);
    put(gpu::kMPsFinalConstant1, d[44]);
    put(gpu::kMPsCombinerCount, d[53]);
    put(gpu::kMPsDotMapping, d[55]);
    put(gpu::kMPsInputTexture, d[56]);
    SetComplex(gpu::kRsPsTextureModes, d[54]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShaderConstant, 3) {
    WalkBeforeState();
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t reg = a[0], src = a[1], n = a[2];
    for (uint32_t i = 0; i < n && reg + i < 16; i++) std::memcpy(g_state.psc[reg + i].data(), HostPtr(src + i * 16), 16);
    gpu::ApplyPixelConstants(g_state, reg, n);
    return 0;
}


// XBE_FRAMELOG=N: the next N frames' draws into logs/framelog.txt at full speed, one line each: frame, draw, shader,
// primitive, count, texture data, and hashes of the vertex bytes the draw reads, its indices, each texture's level 0
// (up to 64 KB) and the stage, pixel constant and deferred render states. Static scenery hashes the same every frame:
// what alternates between frames shows in which input it alternates.
namespace {
uint64_t Fnv(const uint8_t* p, size_t n, uint64_t h = 1469598103934665603ull) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
FILE* g_framelog = nullptr;  // open while frames are being logged
uint32_t g_framelog_first = 0;
// a texture whose data is physical address 0: a header the title filled from something the host left empty. Its
// address and fields, once each
void WarnEmptyTextures() {
    for (int i = 0; i < 4; i++)
        if (uint32_t t = g_state.textures[i]; t && (R32(t + 4) & 0x0FFFFFFF) == 0) {
            static std::unordered_set<uint32_t> told;
            if (told.size() < 16 && told.insert(t).second)
                LOGW("d3d: stage %d texture %08X points at physical address 0 (common %08X, format %08X, size %08X, frame %u)",
                     i, t, R32(t), R32(t + 12), R32(t + 16), uint32_t(g_presents));
        }
}
void FrameLogDraw(uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    WarnEmptyTextures();
    static const long frames = [] {
        const char* e = std::getenv("XBE_FRAMELOG");
        return e && *e ? std::atol(e) : 0L;
    }();
    if (frames <= 0 || !count) return;
    FILE*& f = g_framelog;
    uint32_t& first = g_framelog_first;
    static uint32_t last_frame = ~0u, draw = 0;
    uint32_t frame = g_presents.load();
    if (!f) {
        // F4, or a file named framelog beside the exe (looked for every 30 frames), starts it; again for the next
        // file (framelog.txt, framelog_2.txt, ...)
        if (!diag::g_framelog_request && frame % 30 == 0 && frame != last_frame) {
            last_frame = frame;
            std::error_code ec;
            if (std::filesystem::remove(app::ExeDir() / "framelog", ec)) diag::g_framelog_request = true;
        }
        if (!diag::g_framelog_request.exchange(false)) return;
        static int run = 0;
        std::string name = ++run == 1 ? "framelog.txt" : "framelog_" + std::to_string(run) + ".txt";
        f = std::fopen((app::ExeDir() / "logs" / name).string().c_str(), "w");
        if (!f) return;
        first = frame;
        LOGR("d3d: XBE_FRAMELOG writing %ld frames to logs/%s", frames, name.c_str());
    }
    if (frame - first >= uint32_t(frames)) {
        std::fclose(f);
        f = nullptr;
        LOGR("d3d: XBE_FRAMELOG done");
        return;
    }
    if (frame != last_frame) {
        last_frame = frame;
        draw = 0;
    }
    // the vertex range read
    uint32_t lo = start, hi = start + count - 1;
    uint64_t ih = 0;
    if (indices) {
        lo = 0xFFFFFFFF;
        hi = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t v = uint32_t(indices[i * 2]) | uint32_t(indices[i * 2 + 1]) << 8;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        lo += g_state.base_vertex;
        hi += g_state.base_vertex;
        ih = Fnv(indices, size_t(count) * 2);
    }
    const auto& streams = g_state.input_active ? g_state.input_streams : g_state.streams;
    uint64_t vh = 1469598103934665603ull;
    for (const auto& st : streams) {
        if (!st.vb || !st.stride || hi - lo > 65535) continue;
        uint32_t data = mem::VirtualFromPhysical(R32(st.vb + 4));
        if (!mem::IsCommitted(data + lo * st.stride) || !mem::IsCommitted(data + (hi + 1) * st.stride - 1)) continue;
        vh = Fnv(static_cast<const uint8_t*>(HostPtr(data + lo * st.stride)), size_t(hi - lo + 1) * st.stride, vh);
    }
    uint64_t th[4] = {};
    uint32_t td[4] = {};
    for (int i = 0; i < 4; i++) {
        gpu::TextureDesc d;
        if (!g_state.textures[i] || !gpu::Describe(g_state.textures[i], d)) continue;
        td[i] = d.data;
        uint32_t n = std::min<uint32_t>(gpu::LevelSize(d, 0), 65536);
        if (n && mem::IsCommitted(d.data) && mem::IsCommitted(d.data + n - 1)) th[i] = Fnv(static_cast<const uint8_t*>(HostPtr(d.data)), n);
    }
    uint64_t tssh = Fnv(reinterpret_cast<const uint8_t*>(g_state.tss.data()), sizeof(g_state.tss));
    // each stage table the first time it is seen: "tssdef hash stage: values by D3DTSS index"
    static std::unordered_set<uint64_t> tss_seen;
    if (tss_seen.insert(tssh).second) {
        std::fprintf(f, "tssdef %016llX", (unsigned long long)tssh);
        for (int st = 0; st < 4; st++) {
            std::fprintf(f, " |%d:", st);
            for (uint32_t i = 0; i < gpu::kTssCount; i++) std::fprintf(f, " %X", g_state.tss[st][i]);
        }
        std::fprintf(f, "\n");
    }
    uint64_t rsh = Fnv(reinterpret_cast<const uint8_t*>(&g_state.rs[gpu::kRsDeferredFirst]), (gpu::kRsComplexFirst - gpu::kRsDeferredFirst) * 4);
    std::fprintf(f, "%u %u ps %08X vs %08X prim %u n %u rt %08X tex %08X %08X %08X %08X vb %016llX ib %016llX th %016llX %016llX %016llX %016llX tss %016llX rs %016llX",
                 frame - first, draw++, g_state.ps ? g_state.ps->handle : 0u, g_state.vs_handle, prim, count, g_state.rt.data, td[0], td[1], td[2], td[3],
                 (unsigned long long)vh, (unsigned long long)ih, (unsigned long long)th[0], (unsigned long long)th[1],
                 (unsigned long long)th[2], (unsigned long long)th[3], (unsigned long long)tssh, (unsigned long long)rsh);
    // the pixel shader constants as values, the vertex constants as a hash per block of 16 registers (c-96 up)
    std::fprintf(f, " psc");
    auto byte = [](float v) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
    for (const auto& c : g_state.psc) std::fprintf(f, " %08X", byte(c[0]) << 24 | byte(c[1]) << 16 | byte(c[2]) << 8 | byte(c[3]));
    std::fprintf(f, " vsc");
    for (size_t b = 0; b < 12; b++)
        std::fprintf(f, " %08X", uint32_t(Fnv(reinterpret_cast<const uint8_t*>(&g_state.vsc[b * 16]), 16 * 16)));
    std::fprintf(f, "\n");
}
}  // namespace

namespace d3d {
// a line from the title's code into the frame log while it is being written ("note" lines between the draws)
void FrameLogNote(const char* text) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_framelog) std::fprintf(g_framelog, "note %u %s\n", g_presents.load() - g_framelog_first, text);
}
}  // namespace d3d

// drawing

// Push buffers recorded to run later. The host draws at the API, so a push buffer the title records holds only the
// commands it pushed itself; the draws made while it recorded are kept here, each with the state it had and where
// in the buffer it came, and drawn again when the buffer runs (RunPushBuffer), between its commands. A buffer the
// title records and runs in the same frame, twice (a whole frame recorded, as 007: Everything or Nothing does), is
// drawn as it records and not kept.
namespace {
struct PbDraw {
    uint32_t offset = 0;  // in the buffer's data
    gpu::State state;     // the vertex constants are the replay's, which the buffer's commands (and fixups) load
    uint32_t prim = 0, start = 0, count = 0;
    uint32_t indices = 0;     // guest address, 0 for none
    std::vector<uint8_t> up;  // DrawUp's vertices
    uint32_t up_stride = 0;
};
struct PbRecording {
    std::vector<std::unique_ptr<PbDraw>> draws;
    uint32_t ended = ~0u;      // the present it was recorded in
    uint32_t same_frame = 0;   // times it ran in the frame it was recorded in
    bool immediate = false;    // drawn as it records
    bool overflow = false;
};
constexpr size_t kPbMaxDraws = 4096;
std::unordered_map<uint32_t, PbRecording> g_pb;
uint32_t g_pb_recording = 0, g_pb_data = 0;  // the buffer being recorded and its data
bool g_pb_keep = false;                      // its draws are kept, not drawn

// true when the draw is kept for the buffer being recorded instead of drawn now
bool KeepDraw(uint32_t prim, uint32_t start, uint32_t count, uint32_t indices, const void* up = nullptr,
              uint32_t up_stride = 0) {
    if (!g_pb_keep) return false;
    PbRecording& r = g_pb[g_pb_recording];
    if (r.draws.size() >= kPbMaxDraws) {
        r.overflow = true;
        return true;
    }
    auto d = std::make_unique<PbDraw>();
    d->offset = Dev(kDevPush) - g_pb_data;
    d->state = g_state;
    d->prim = prim, d->start = start, d->count = count, d->indices = indices;
    if (up) {
        d->up.assign(static_cast<const uint8_t*>(up), static_cast<const uint8_t*>(up) + count * up_stride);
        d->up_stride = up_stride;
    }
    r.draws.push_back(std::move(d));
    return true;
}
}  // namespace

HLE_STDCALL(D3DDevice_DrawVertices, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // a visibility test begun since the last draw counts this one
    SyncDeferred();
    if (KeepDraw(a[0], a[1], a[2], 0)) return 0;
    FrameLogDraw(a[0], a[1], a[2], nullptr);
    if (g_backend) g_backend->Draw(g_state, a[0], a[1], a[2], nullptr);
    return 0;
}

HLE_STDCALL(D3DDevice_DrawIndexedVertices, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    SyncDeferred();
    if (KeepDraw(a[0], 0, a[1], a[2])) return 0;
    const uint8_t* indices = a[2] ? static_cast<const uint8_t*>(HostPtr(a[2])) : nullptr;
    FrameLogDraw(a[0], 0, a[1], indices);
    if (g_backend) g_backend->Draw(g_state, a[0], 0, a[1], indices);
    return 0;
}

// DrawVerticesUP(prim, count, vertices, stride): the vertices are copied to a ring the render thread reads, stream 0
// points there through a vertex buffer header for the draw. A half of the ring is written again once the draws
// that read it are done.
namespace {
constexpr uint32_t kUpRing = 4u << 20, kUpHalf = kUpRing / 2;
uint32_t g_up_ring = 0, g_up_pos = 0, g_up_header = 0, g_up_fence[2] = {};

uint32_t UpVertices(const void* src, uint32_t bytes) {
    if (!g_up_ring) {
        g_up_ring = mem::AllocHostContiguous(kUpRing, 0x1000);
        g_up_header = mem::AllocSystem(20);
        std::memset(HostPtr(g_up_header), 0, 20);
        W32(g_up_header, 1);  // a vertex buffer, one reference
    }
    bytes = std::min(bytes, kUpHalf);
    uint32_t room = (bytes + 63) & ~63u;
    uint32_t pos = g_up_pos + room > kUpRing ? 0 : g_up_pos;
    uint32_t from = g_up_pos ? (g_up_pos - 1) / kUpHalf : 1, to = (pos + room - 1) / kUpHalf;
    if (to != from) {
        g_up_fence[from] = g_backend->Fence();
        g_backend->WaitFence(g_up_fence[to]);
    }
    std::memcpy(HostPtr(g_up_ring + pos), src, bytes);
    g_up_pos = pos + room;
    W32(g_up_header + 4, mem::PhysicalFromVirtual(g_up_ring + pos));
    return g_up_header;
}

void DrawUp(uint32_t prim, uint32_t count, const void* vertices, uint32_t stride) {
    if (!g_backend || !count || !stride) return;
    SyncDeferred();
    WarnEmptyTextures();
    if (KeepDraw(prim, 0, count, 0, vertices, stride)) return;
    auto stream = g_state.streams[0];
    bool input = g_state.input_active;  // vertices from memory are read by the shader's declaration
    g_state.streams[0] = {UpVertices(vertices, count * stride), stride};
    g_state.input_active = false;
    g_backend->Draw(g_state, prim, 0, count, nullptr);
    g_state.streams[0] = stream;
    g_state.input_active = input;
}

// bytes per vertex of the bound format: stream 0 of the FVF or of the shader's declaration
uint32_t VertexBytes() {
    if (g_state.vs_handle & 1) return g_state.vs ? g_state.vs->stream_stride[0] : 0;
    gpu::VertexShader fvf;
    gpu::FvfInputs(g_state.vs_handle, fvf);
    return fvf.stream_stride[0];
}

uint32_t g_push_begin = 0;
}  // namespace

// Begin/End immediate mode. Titles inline Begin and the vertex data calls, so their commands are read from the scratch buffer.
namespace d3d {
namespace {
uint32_t g_im_prim = 0, g_im_mask = 0;  // open primitive, slots it wrote
float g_im_attr[64];                    // 16 slots of x y z w
std::vector<float> g_im_vertices;       // every slot per vertex
std::vector<uint16_t> g_im_indices;     // array elements (0x1800, 0x1808): drawn from the bound vertex data
// triangles of pushed indexed strips, fans and lists in a row, drawn as one list: a model built as a push buffer is
// hundreds of short strips with no state between them (007: Everything or Nothing's characters, over half its draws)
std::vector<uint16_t> g_im_batch;

void FlushPushedTriangles() {
    if (g_im_batch.empty()) return;
    if (g_backend) {
        auto idx = reinterpret_cast<const uint8_t*>(g_im_batch.data());
        uint32_t count = uint32_t(g_im_batch.size());
        FrameLogDraw(5, 0, count, idx);
        g_backend->Draw(g_state, 5, 0, count, idx);
    }
    g_im_batch.clear();
}

// a strip (6), fan (7) or list (5) of indices as list triangles with the console's winding; degenerate ones dropped
void BatchPushedTriangles(uint32_t prim, const std::vector<uint16_t>& in) {
    auto add = [](uint16_t a, uint16_t b, uint16_t c) {
        if (a == b || b == c || a == c) return;
        g_im_batch.insert(g_im_batch.end(), {a, b, c});
    };
    size_t n = in.size();
    if (prim == 5)
        for (size_t i = 0; i + 2 < n; i += 3) add(in[i], in[i + 1], in[i + 2]);
    else if (prim == 6)
        for (size_t i = 0; i + 2 < n; i++) i & 1 ? add(in[i + 1], in[i], in[i + 2]) : add(in[i], in[i + 1], in[i + 2]);
    else
        for (size_t i = 1; i + 1 < n; i++) add(in[0], in[i], in[i + 1]);
}

// visibility tests: depth passing pixels are counted while enabled, a report gets the count since the clear,
// at a physical address in the first 16 MB
void ApplyVisibilityMethod(uint32_t m, uint32_t v) {
    constexpr uint32_t kReportAddressMask = 0xFFFFFF;
    if (m == gpu::kMCountEnable) {
        g_state.simple[gpu::kMCountEnable >> 2] = v;
        if (g_framelog) d3d::FrameLogNote(v ? "visibility count on" : "visibility count off");
    }
    if (g_framelog && m == gpu::kMGetReport) d3d::FrameLogNote("visibility report");
    if (!g_backend) return;
    if (m == gpu::kMClearReport) g_backend->ClearReport();
    if (m == gpu::kMGetReport) {
        // XBE_VISIBLE_ALL=1 (experiment): every test written at once as visible (time, count, then 0 for done)
        static const bool all = [] {
            const char* e = std::getenv("XBE_VISIBLE_ALL");
            bool on = e && *e == '1';
            if (on) LOGR("d3d: XBE_VISIBLE_ALL=1, every visibility test reports visible at once");
            return on;
        }();
        uint32_t report = mem::VirtualFromPhysical(v & kReportAddressMask);
        if (all) {
            W32(report, 0);
            W32(report + 4, 0);
            W32(report + 8, 0x100000);
            W32(report + 12, 0);
            return;
        }
        g_backend->GetReport(report);
        g_report_asked = true;
    }
}

// vertex shader constants pushed as raw commands: the load slot, then dwords written through the window from it.
// The slot moves on by the constants of each method group, so a group of 16 dwords at the window start loads 4.
uint32_t g_constant_load = 0;
bool ApplyConstantMethod(uint32_t m, uint32_t v) {
    if (m == gpu::kMConstantLoad) {
        g_constant_load = v;
        return true;
    }
    if (m < gpu::kMConstant || m >= gpu::kMConstantEnd) return false;
    uint32_t slot = g_constant_load + (m - gpu::kMConstant) / 16;
    if (slot < g_state.vsc.size()) std::memcpy(&g_state.vsc[slot][m / 4 & 3], &v, 4);
    if ((m / 4 & 3) == 3) WatchConstants("push buffer", slot, 1, 0);
    return true;
}
void EndConstantGroup(uint32_t method, uint32_t n) {
    if (method >= gpu::kMConstant && method < gpu::kMConstantEnd) g_constant_load += n / 4;
}

// draws the open primitive: the slots it wrote are FLOAT4 inputs of the bound program, or of the fixed pipeline
void EndPrimitive() {
    struct Made {
        std::shared_ptr<gpu::VertexShader> from, vs;
    };
    // the render thread tells shaders apart by pointer: one object per program and slot set, the program kept alive
    static std::map<std::pair<gpu::VertexShader*, uint32_t>, Made> made;
    static std::vector<float> packed;
    uint32_t prim = g_im_prim, count = uint32_t(g_im_vertices.size() / 64), handle = g_state.vs_handle;
    g_im_prim = 0;
    if (!prim || !count) return;
    if (!(handle & 1)) {
        // an FVF: the slots packed in its own layout, drawn by the fixed pipeline (pretransformed XYZRHW included).
        // Immediate data goes to the vertex registers whatever the FVF says (a movie player may give texture
        // coordinates under XYZRHW | DIFFUSE), so the FVF is widened by the slots written: diffuse 3, specular 4, texture
        // coordinates 9 to 12 as four floats. A D3DCOLOR goes back to its bytes B, G, R, A from the slot's R, G, B, A.
        constexpr uint32_t kColor = 0x40, kFvfDiffuse = 0x40, kFvfSpecular = 0x80, kTexSlot = 9, kTexFloat4 = 2;
        uint32_t fvf_handle = handle;
        if (g_im_mask >> 3 & 1) fvf_handle |= kFvfDiffuse;
        if (g_im_mask >> 4 & 1) fvf_handle |= kFvfSpecular;
        uint32_t declared = fvf_handle >> 8 & 15, written = 0;
        for (uint32_t t = 0; t < 4; t++)
            if (g_im_mask >> (kTexSlot + t) & 1) written = t + 1;
        if (written > declared) {
            for (uint32_t t = declared; t < written; t++) fvf_handle |= kTexFloat4 << (16 + t * 2);
            fvf_handle = (fvf_handle & ~0xF00u) | written << 8;
        }
        gpu::VertexShader fvf;
        gpu::FvfInputs(fvf_handle, fvf);
        uint32_t stride = fvf.stream_stride[0];
        static std::vector<uint8_t> bytes;
        bytes.assign(size_t(stride) * count, 0);
        for (uint32_t v = 0; v < count; v++)
            for (uint32_t r = 0; r < 16; r++) {
                const auto& in = fvf.inputs[r];
                if (in.stream < 0) continue;
                const float* slot = &g_im_vertices[v * 64 + r * 4];
                uint8_t* out = bytes.data() + size_t(v) * stride + in.offset;
                if (in.type == kColor) {
                    for (int c = 0; c < 4; c++)
                        out[c] = uint8_t(std::lround(std::clamp(slot[c == 3 ? 3 : 2 - c], 0.0f, 1.0f) * 255.0f));
                } else {
                    std::memcpy(out, slot, std::min<uint32_t>(in.type >> 4, 4) * 4);
                }
            }
        g_state.vs_handle = fvf_handle;
        DrawUp(prim, count, bytes.data(), stride);
        g_state.vs_handle = handle;
        return;
    }
    auto bound = g_state.vs, from = handle & 1 ? bound : nullptr;
    Made& m = made[{from.get(), g_im_mask}];
    if (!m.vs) {
        m.from = from;
        m.vs = std::make_shared<gpu::VertexShader>();
        if (from) {
            m.vs->program = from->program;
            m.vs->program_type = from->program_type;
        }
        for (uint32_t s = 0; s < 16; s++)
            if (g_im_mask >> s & 1) {
                m.vs->inputs[s] = {0, m.vs->stream_stride[0], 0x42};
                m.vs->stream_stride[0] += 16;
            }
    }
    packed.clear();
    for (uint32_t v = 0; v < count; v++)
        for (uint32_t s = 0; s < 16; s++)
            if (g_im_mask >> s & 1) packed.insert(packed.end(), &g_im_vertices[v * 64 + s * 4], &g_im_vertices[v * 64 + s * 4] + 4);
    g_state.vs = m.vs;
    g_state.vs_handle = handle | 1;
    DrawUp(prim, count, packed.data(), m.vs->stream_stride[0]);
    g_state.vs = bound;
    g_state.vs_handle = handle;
}

// reads the commands pushed since the last call: vertex data between a begin and an end is drawn, visibility tests
// are run, vertex shader constants are loaded, the rest is skipped
void Walk(uint32_t end) {
    if (!g_walk || end < g_walk || end > g_walk_limit) return;
    for (uint32_t p = g_walk; p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p), method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || n * 4 > end - p) break;  // anything but a method header
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4, v = R32(p);
            if (!g_im_batch.empty() && m != 0x17FC && m != 0x1800 && m != 0x1808) FlushPushedTriangles();  // state first
            float f, *a = g_im_attr;
            bool last = true;  // the slot's last component
            std::memcpy(&f, &v, 4);
            ApplyVisibilityMethod(m, v);
            if (ApplyConstantMethod(m, v)) continue;
            if (m == 0x1E9C) {  // program load slot
                g_program_load = std::min(v, kProgramSlots) * 4;
                continue;
            }
            if (m >= 0x0B00 && m < 0x0B80) {  // program instructions
                if (g_program_load < kProgramSlots * 4) g_program[g_program_load++] = v;
                g_program_written = true;
                continue;
            }
            if (m == 0x1800 || m == 0x1808) {  // array elements: two 16 bit indices, or one 32 bit
                if (!g_im_prim) continue;
                if (m == 0x1808) {
                    if (v > 0xFFFF) LOG_FIRST(4, "d3d: pushed array element %u is past 16 bits", v);
                    g_im_indices.push_back(uint16_t(v));
                } else {
                    g_im_indices.push_back(uint16_t(v));
                    g_im_indices.push_back(uint16_t(v >> 16));
                }
                continue;
            }
            if (m == 0x1810) {  // draw arrays: first vertex, count - 1 in the top byte
                if (g_im_prim && g_backend) {
                    SyncDeferred();
                    FrameLogDraw(g_im_prim, v & 0xFFFFFF, (v >> 24) + 1, nullptr);
                    g_backend->Draw(g_state, g_im_prim, v & 0xFFFFFF, (v >> 24) + 1, nullptr);
                }
                continue;
            }
            if (m == 0x17FC) {  // begin/end, 0 ends
                if (v) {
                    g_im_prim = v;
                    g_im_mask = 0;
                    g_im_vertices.clear();
                    g_im_indices.clear();
                } else {
                    uint32_t prim = g_im_prim;
                    RefreshSelectedProgram();
                    EndPrimitive();
                    if (prim && !g_im_indices.empty() && g_backend) {
                        if (g_pb_keep) {
                            LOG_FIRST(4, "d3d: pushed indexed primitive while a push buffer records, not kept");
                        } else if (prim >= 5 && prim <= 7) {
                            SyncDeferred();
                            BatchPushedTriangles(prim, g_im_indices);
                        } else {
                            SyncDeferred();
                            auto idx = reinterpret_cast<const uint8_t*>(g_im_indices.data());
                            uint32_t count = uint32_t(g_im_indices.size());
                            FrameLogDraw(prim, 0, count, idx);
                            g_backend->Draw(g_state, prim, 0, count, idx);
                        }
                    }
                    g_im_indices.clear();
                }
                continue;
            } else if (m >= 0x1880 && m < 0x1900) {  // 2 floats, z = 0 and w = 1
                a += (m - 0x1880) / 8 * 4;
                last = (m - 0x1880) / 4 & 1;
                a[last] = f;
                a[2] = 0;
                a[3] = 1;
            } else if (m >= 0x1940 && m < 0x1980) {  // 4 bytes
                a += (m - 0x1940) / 4 * 4;
                for (int c = 0; c < 4; c++) a[c] = (v >> c * 8 & 0xFF) / 255.0f;
            } else if (m >= 0x1A00 && m < 0x1B00) {  // 4 floats
                a += (m - 0x1A00) / 16 * 4;
                last = ((m - 0x1A00) / 4 & 3) == 3;
                a[(m - 0x1A00) / 4 & 3] = f;
            } else if (m >= 0x1500 && m < 0x150C) {  // position, 3 floats and w = 1
                last = m == 0x1508;
                a[(m - 0x1500) / 4] = f;
                a[3] = 1;
            } else if (m >= 0x1518 && m < 0x1528) {  // position, 4 floats
                last = m == 0x1524;
                a[(m - 0x1518) / 4] = f;
            } else {
                continue;
            }
            if (!g_im_prim) continue;
            g_im_mask |= 1u << (a - g_im_attr) / 4;
            if (a == g_im_attr && last) g_im_vertices.insert(g_im_vertices.end(), g_im_attr, g_im_attr + 64);
        }
        EndConstantGroup(method, n);
    }
    FlushPushedTriangles();
    RefreshSelectedProgram();
    g_walk = end;
}
}  // namespace
}  // namespace d3d

HLE_STDCALL(D3DDevice_DrawVerticesUP, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    DrawUp(a[0], a[1], HostPtr(a[2]), a[3]);
    return 0;
}

// BeginPushBuffer(push buffer) records what follows into the title's buffer: the library's own code points the
// device's push at it, and Walk reads the commands there as the title writes them, in order with the calls the host
// draws at once (007: Everything or Nothing pushes its vertex programs and constants while it records a frame).
// EndPushBuffer: the rest of the buffer read, the push back where it was.
#if HAS_D3DDevice_BeginPushBuffer && HAS_D3DDevice_EndPushBuffer
X86_EXTERN_FUNC(GUEST_ORIG(D3DDevice_BeginPushBuffer));
X86_FUNC(GUEST(D3DDevice_BeginPushBuffer)) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t pb = R32(ctx.esp + 4);
    Walk(Dev(kDevPush));
    GUEST_ORIG(D3DDevice_BeginPushBuffer)(ctx, base);
    g_walk = Dev(kDevPush);
    g_walk_limit = R32(pb + 4) + R32(pb + 0x10);  // Data, AllocationSize
    PbRecording& r = g_pb[pb];
    g_pb_recording = pb;
    g_pb_data = R32(pb + 4);
    g_pb_keep = !r.immediate;
    r.draws.clear();
    r.overflow = false;
}
X86_EXTERN_FUNC(GUEST_ORIG(D3DDevice_EndPushBuffer));
X86_FUNC(GUEST(D3DDevice_EndPushBuffer)) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    GUEST_ORIG(D3DDevice_EndPushBuffer)(ctx, base);
    g_walk = Dev(kDevPush);
    g_walk_limit = g_scratch_push + g_scratch_size;
    if (g_pb_recording) g_pb[g_pb_recording].ended = g_presents;
    g_pb_recording = 0;
    g_pb_keep = false;
}
#endif

// RunPushBuffer(push buffer, fixup): a buffer whose draws were kept (above) is drawn: the fixup's batch is copied
// into it (records of byte count, offset in the data, then the commands, as D3DPushBuffer_SetVertexShaderConstant
// writes them while a fixup is begun), then its commands are read in order with the kept draws, each drawn with its
// own state and the vertex constants the commands left. Any other is logged once (its size, Common and first
// commands) and not run.
namespace {
void ReplayPushBuffer(uint32_t pb, uint32_t fixup, PbRecording& r) {
    uint32_t data = R32(pb + 4), size = R32(pb + 12);  // Data, Size
    if (fixup) {
        uint32_t p = R32(fixup + 4) + R32(fixup + 12), end = R32(fixup + 4) + R32(fixup + 16);  // Data + Run, + Next
        while (p + 8 <= end) {
            uint32_t bytes = R32(p), at = R32(p + 4);
            if (!bytes || bytes == 0xFFFFFFFFu || p + 8 + bytes > end) break;
            if (at + bytes <= size) std::memcpy(HostPtr(data + at), HostPtr(p + 8), bytes);
            p += 8 + bytes;
        }
    }
    uint32_t walk = g_walk, limit = g_walk_limit;
    g_walk = data;
    g_walk_limit = data + size;
    for (auto& d : r.draws) {
        Walk(data + d->offset);
        gpu::State& s = d->state;
        s.vsc = g_state.vsc;
        if (KeepDraw(d->prim, d->start, d->count, d->indices, d->up.empty() ? nullptr : d->up.data(), d->up_stride))
            continue;  // run while another buffer records: kept for that one
        // the textures it binds, as a title watching its binds would see them drawn (EoN's title art is replayed)
        if (app::kGame.texture_bound)
            for (size_t t = 0; t < s.textures.size(); t++)
                if (s.textures[t] && app::kGame.texture_bound(s.textures[t], s.palettes[t])) gpu::MarkDirty(s.textures[t]);
        // and what a title's hooks ask now (gpu::State::squeeze_beyond, blank_format; set as its binds are seen)
        s.squeeze_beyond = g_state.squeeze_beyond;
        s.squeeze_shift = g_state.squeeze_shift;
        s.blank_format = g_state.blank_format;
        if (!g_backend) continue;
        if (!d->up.empty()) {
            s.streams[0] = {UpVertices(d->up.data(), uint32_t(d->up.size())), d->up_stride};
            s.input_active = false;
            g_backend->Draw(s, d->prim, 0, d->count, nullptr);
        } else {
            g_backend->Draw(s, d->prim, d->start, d->count,
                            d->indices ? static_cast<const uint8_t*>(HostPtr(d->indices)) : nullptr);
        }
    }
    Walk(data + size);
    g_walk = walk;
    g_walk_limit = limit;
}
}  // namespace

HLE_STDCALL(D3DDevice_RunPushBuffer, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    uint32_t pb = a[0];
    if (auto it = g_pb.find(pb); it != g_pb.end()) {
        PbRecording& r = it->second;
        if (r.immediate) return 0;  // drawn as it recorded
        if (r.ended == g_presents && ++r.same_frame >= 2) {
            r.immediate = true;
            LOGI("d3d: push buffer %08X is recorded and run in the same frame, drawn as it records from now on", pb);
        }
        if (r.overflow) LOG_FIRST(8, "d3d: push buffer %08X kept only its first %zu draws", pb, kPbMaxDraws);
        LOG_FIRST(16, "d3d: RunPushBuffer %08X replays %zu draws (fixup %08X)", pb, r.draws.size(), a[1]);
        ReplayPushBuffer(pb, a[1], r);
        return 0;
    }
    // one the title made itself (built offline and loaded): its commands, fixed up, are all there is
    if (pb && mem::IsCommitted(pb) && mem::IsCommitted(pb + 15) && R32(pb + 4) && mem::IsCommitted(R32(pb + 4)) &&
        mem::IsCommitted(R32(pb + 4) + std::max<uint32_t>(R32(pb + 12), 1) - 1)) {
        static PbRecording none;
        LOG_FIRST(8, "d3d: RunPushBuffer %08X: a buffer not recorded here, %u bytes (fixup %08X), its commands read", pb,
                  R32(pb + 12), a[1]);
        ReplayPushBuffer(pb, a[1], none);
        return 0;
    }
    static std::unordered_set<uint32_t> seen;
    if (pb && mem::IsCommitted(pb) && mem::IsCommitted(pb + 15) && seen.size() < 64 && seen.insert(pb).second) {
        uint32_t data = R32(pb + 4), size = R32(pb + 12);
        // the first commands, only from memory that is there (the title may give a physical or unfinished address)
        bool readable = data && mem::IsCommitted(data) && mem::IsCommitted(data + 31);
        char head[200] = {};
        for (uint32_t i = 0, n = 0; i < 8 && i * 4 < size && readable; i++)
            n += std::snprintf(head + n, sizeof head - n, " %08X", R32(data + i * 4));
        LOGW("d3d: RunPushBuffer %08X (Common %08X, data %08X, %u bytes, fixup %08X) not replayed:%s", pb, R32(pb), data,
             size, a[1], head);
    }
    return 0;
}

// BeginPush(dwords) hands out room for raw hardware commands, EndPush(end) runs what the title wrote there:
// a primitive between two begin/end writes with its vertices inline, vertex shader constants, any other method as a
// simple state.
HLE_STDCALL(D3DDevice_BeginPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_push_begin = ScratchPush(a[0] * 4);
    return g_push_begin;
}

HLE_STDCALL(D3DDevice_EndPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    constexpr uint32_t kBeginEnd = 0x17FC, kInlineArray = 0x1818;
    static std::vector<uint32_t> vertices;
    uint32_t prim = 0;
    for (uint32_t p = g_push_begin, end = a[0]; g_push_begin && p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p), method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || n * 4 > end - p) {  // anything but a method header
            LOG_FIRST(8, "d3d: EndPush stops at command %08X, %u bytes before the end", head, end - p);
            break;
        }
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4, v = R32(p);
            ApplyVisibilityMethod(m, v);
            if (ApplyConstantMethod(m, v)) continue;
            if (m == kInlineArray) {
                if (prim) vertices.push_back(v);
            } else if (m != kBeginEnd) {
                if (m >> 2 < g_state.simple.size()) g_state.simple[m >> 2] = v;
            } else if (v) {
                prim = v;
                vertices.clear();
            } else if (prim) {
                uint32_t stride = VertexBytes(), bytes = uint32_t(vertices.size()) * 4;
                if (stride && bytes % stride == 0) DrawUp(prim, bytes / stride, vertices.data(), stride);
                else LOG_FIRST(8, "d3d: EndPush primitive %u dropped, %u bytes of vertices of %u", prim, bytes, stride);
                prim = 0;
            }
        }
        EndConstantGroup(method, n);
    }
    g_push_begin = 0;
    ScratchPush();
    return a[0];
}

// frame buffer compression tiles are the console's memory controller's business
// (index, tile): there is no tiled memory here, but the record is kept where the library keeps it: titles read it
// back with GetTile (007: Everything or Nothing finds its depth buffer's memory so, for its weapon carousel)
HLE_STDCALL(D3DDevice_SetTile, 2) {
    constexpr uint32_t kTileBytes = 24, kTiles = 8;
    if (XDK_CDevice_Tiles != xdk::kUnknown && a[0] < kTiles)
        for (uint32_t i = 0; i < kTileBytes; i += 4) W32(kDevice + XDK_CDevice_Tiles + a[0] * kTileBytes + i, a[1] ? R32(a[1] + i) : 0);
    return 0;
}

// the library's Begin pushes the start of the primitive, here it is opened
HLE_STDCALL(D3DDevice_Begin, 1) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    g_im_prim = a[0];
    g_im_mask = 0;
    g_im_vertices.clear();
    SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) | 0x800);
    return 0;
}
// the library's End pushes the end of the primitive, here it is drawn
HLE_STDCALL(D3DDevice_End, 0) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    EndPrimitive();
    SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) & ~0x1800u);
    return 0;
}
