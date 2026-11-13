// The title's Direct3D 8 calls drawn with Metal (macOS). The console path of d3d11.cpp, with what does not depend on
// the API shared through d3d_common; its HLSL compiled to SPIR-V by glslang and on to Metal's shading language by
// SPIRV-Cross. One command buffer at a time is recorded; a render pass is open while draws go to the same targets, and
// anything else (copies, uploads, read-backs, passes into other targets) ends it.
// MSAA as d3d11.cpp has it: draws into a multisampled twin of a target, resolved by the render pass's store action
// (colour, and depth and stencil from sample 0) when something reads the target.
// Metal's samplers on macOS have no mip LOD bias: the pixel shaders add it (g_sample_bias). A line is drawn one
// console pixel wide by drawing it again moved by whole host pixels, where d3d11.cpp has a geometry shader.
// Not here yet: F3 captures, renderer extensions, late read-backs (a NOOVERWRITE lock waits).
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <array>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <SDL3/SDL.h>
#define ENABLE_HLSL 1  // the HLSL front end (homebrew glslang is built with it)
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <spirv_cross/spirv_msl.hpp>

#include <imgui.h>
#include <backends/imgui_impl_metal.h>
#include <backends/imgui_impl_sdl3.h>

#include "app/app.h"
#include "core/diag.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/d3d_common.h"
#include "gpu/shaders.h"
#include "gpu/smaa.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"

namespace app {
void* MetalLayer();  // app/main_sdl.cpp: the window's CAMetalLayer
}

namespace gpu {

namespace {

constexpr MTLPixelFormat kBgra = MTLPixelFormatBGRA8Unorm;
// Apple GPUs have no 24 bit depth: 32 bit float depth with 8 bit stencil
constexpr MTLPixelFormat kDepthFormat = MTLPixelFormatDepth32Float_Stencil8;
constexpr uint64_t kStaleFrames = 8;
constexpr uint32_t kStaleReadMax = 1 << 20;
constexpr uint32_t kVertexBufferBase = 8;   // buffer indices: 0 and 1 the constant buffers, vertex streams from here
constexpr uint32_t kChunkSize = 8u << 20;   // upload memory, handed out per command buffer
constexpr uint32_t kVisibilitySlots = 4096; // counted draws per command buffer
constexpr int kInFlight = 3;                // command buffers the CPU may be ahead by

// ---- HLSL to Metal

std::once_flag g_glslang_once;

struct Compiled {
    id<MTLFunction> fn = nil;
    int8_t attr_reg[16];  // vertex shaders: input attribute location to v register, -1 none
};

// the HLSL's registers bN, tN, sN to Metal's buffer, texture and sampler N; vertex streams stay clear of them
std::string HlslToMsl(const std::string& src, const char* entry, bool vertex, float point_size, int8_t attr_reg[16],
                      std::string& error) {
    std::call_once(g_glslang_once, [] { glslang::InitializeProcess(); });
    const EShLanguage stage = vertex ? EShLangVertex : EShLangFragment;
    glslang::TShader sh(stage);
    const char* text = src.c_str();
    int len = int(src.size());
    sh.setStringsWithLengths(&text, &len, 1);
    sh.setEntryPoint(entry);
    sh.setSourceEntryPoint(entry);
    sh.setEnvInput(glslang::EShSourceHlsl, stage, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    sh.setHlslIoMapping(true);
    sh.setAutoMapLocations(true);
    const EShMessages msgs = EShMessages(EShMsgReadHlsl | EShMsgSpvRules | EShMsgVulkanRules | EShMsgHlslLegalization);
    if (!sh.parse(GetDefaultResources(), 100, false, msgs)) {
        error = sh.getInfoLog();
        return {};
    }
    glslang::TProgram prog;
    prog.addShader(&sh);
    if (!prog.link(msgs) || !prog.mapIO()) {
        error = prog.getInfoLog();
        return {};
    }
    std::vector<uint32_t> spirv;
    glslang::SpvOptions so;
    so.disableOptimizer = false;
    so.optimizeSize = false;
    glslang::GlslangToSpv(*prog.getIntermediate(stage), spirv, &so);

    try {
        spirv_cross::CompilerMSL msl(std::move(spirv));
        spirv_cross::CompilerMSL::Options o;
        o.platform = spirv_cross::CompilerMSL::Options::macOS;
        o.set_msl_version(2, 3);
        o.enable_point_size_builtin = point_size > 0;
        o.enable_point_size_default = point_size > 0;
        o.default_point_size = point_size > 0 ? point_size : 1.0f;
        o.enable_decoration_binding = false;
        msl.set_msl_options(o);
        auto model = vertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment;
        for (uint32_t n = 0; n < 16; n++) {
            spirv_cross::MSLResourceBinding b;
            b.stage = model;
            b.desc_set = 0;
            b.binding = n;
            b.msl_buffer = n;
            b.msl_texture = n;
            b.msl_sampler = n;
            msl.add_msl_resource_binding(b);
        }
        std::fill(attr_reg, attr_reg + 16, int8_t(-1));
        if (vertex) {
            // the generated inputs are v<register> (VIn.v0 ATTR0 ...): each location's register by its name
            for (const auto& in : msl.get_shader_resources().stage_inputs) {
                uint32_t loc = msl.get_decoration(in.id, spv::DecorationLocation);
                const std::string& name = in.name;
                size_t v = name.find_last_of('v');
                if (loc < 16 && v != std::string::npos && v + 1 < name.size() && std::isdigit(uint8_t(name[v + 1])))
                    attr_reg[loc] = int8_t(std::atoi(name.c_str() + v + 1));
            }
        }
        return msl.compile();
    } catch (const std::exception& e) {
        error = e.what();
        return {};
    }
}

// translated shaders kept on disk by source hash (shadercache-metal.bin beside the game), so a shader translated in
// one run is not translated again: glslang and SPIRV-Cross take milliseconds a shader, a hitch the first time a draw
// needs it. Metal keeps its own cache of what it compiled from that source. Each entry: hash, register map, length, MSL
struct MslCache {
    std::mutex m;
    bool loaded = false;
    std::unordered_map<uint64_t, std::pair<std::string, std::array<int8_t, 16>>> entries;
    FILE* out = nullptr;
};

MslCache& Msls() {
    static MslCache c;
    return c;
}

void LoadMslCache(MslCache& c) {
    c.loaded = true;
    const std::filesystem::path file = app::ExeDir() / "shadercache-metal.bin";
    if (FILE* f = std::fopen(file.string().c_str(), "rb")) {
        uint64_t hash;
        std::array<int8_t, 16> regs;
        uint32_t n;
        while (std::fread(&hash, 8, 1, f) == 1 && std::fread(regs.data(), 16, 1, f) == 1 && std::fread(&n, 4, 1, f) == 1 &&
               n < (1u << 24)) {
            std::string msl(n, '\0');
            if (std::fread(msl.data(), 1, n, f) != n) break;
            c.entries[hash] = {std::move(msl), regs};
        }
        std::fclose(f);
    }
    c.out = std::fopen(file.string().c_str(), "ab");
    LOGR("metal: %zu translated shaders kept from earlier runs", c.entries.size());
}

MTLBlendFactor MtlBlend(D3D12_BLEND b) {
    switch (b) {
        case D3D12_BLEND_ZERO: return MTLBlendFactorZero;
        case D3D12_BLEND_ONE: return MTLBlendFactorOne;
        case D3D12_BLEND_SRC_COLOR: return MTLBlendFactorSourceColor;
        case D3D12_BLEND_INV_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
        case D3D12_BLEND_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
        case D3D12_BLEND_INV_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
        case D3D12_BLEND_DEST_ALPHA: return MTLBlendFactorDestinationAlpha;
        case D3D12_BLEND_INV_DEST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
        case D3D12_BLEND_DEST_COLOR: return MTLBlendFactorDestinationColor;
        case D3D12_BLEND_INV_DEST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
        case D3D12_BLEND_SRC_ALPHA_SAT: return MTLBlendFactorSourceAlphaSaturated;
        case D3D12_BLEND_BLEND_FACTOR: return MTLBlendFactorBlendColor;
        case D3D12_BLEND_INV_BLEND_FACTOR: return MTLBlendFactorOneMinusBlendColor;
        default: return MTLBlendFactorOne;
    }
}

MTLBlendOperation MtlBlendOp(D3D12_BLEND_OP o) {
    switch (o) {
        case D3D12_BLEND_OP_SUBTRACT: return MTLBlendOperationSubtract;
        case D3D12_BLEND_OP_REV_SUBTRACT: return MTLBlendOperationReverseSubtract;
        case D3D12_BLEND_OP_MIN: return MTLBlendOperationMin;
        case D3D12_BLEND_OP_MAX: return MTLBlendOperationMax;
        default: return MTLBlendOperationAdd;
    }
}

MTLCompareFunction MtlCmp(D3D12_COMPARISON_FUNC c) {
    switch (c) {
        case D3D12_COMPARISON_FUNC_NEVER: return MTLCompareFunctionNever;
        case D3D12_COMPARISON_FUNC_LESS: return MTLCompareFunctionLess;
        case D3D12_COMPARISON_FUNC_EQUAL: return MTLCompareFunctionEqual;
        case D3D12_COMPARISON_FUNC_LESS_EQUAL: return MTLCompareFunctionLessEqual;
        case D3D12_COMPARISON_FUNC_GREATER: return MTLCompareFunctionGreater;
        case D3D12_COMPARISON_FUNC_NOT_EQUAL: return MTLCompareFunctionNotEqual;
        case D3D12_COMPARISON_FUNC_GREATER_EQUAL: return MTLCompareFunctionGreaterEqual;
        default: return MTLCompareFunctionAlways;
    }
}

MTLStencilOperation MtlStencilOp(D3D12_STENCIL_OP o) {
    switch (o) {
        case D3D12_STENCIL_OP_ZERO: return MTLStencilOperationZero;
        case D3D12_STENCIL_OP_REPLACE: return MTLStencilOperationReplace;
        case D3D12_STENCIL_OP_INCR_SAT: return MTLStencilOperationIncrementClamp;
        case D3D12_STENCIL_OP_DECR_SAT: return MTLStencilOperationDecrementClamp;
        case D3D12_STENCIL_OP_INVERT: return MTLStencilOperationInvert;
        case D3D12_STENCIL_OP_INCR: return MTLStencilOperationIncrementWrap;
        case D3D12_STENCIL_OP_DECR: return MTLStencilOperationDecrementWrap;
        default: return MTLStencilOperationKeep;
    }
}

MTLSamplerAddressMode MtlAddress(D3D12_TEXTURE_ADDRESS_MODE a) {
    switch (a) {
        case D3D12_TEXTURE_ADDRESS_MODE_WRAP: return MTLSamplerAddressModeRepeat;
        case D3D12_TEXTURE_ADDRESS_MODE_MIRROR: return MTLSamplerAddressModeMirrorRepeat;
        case D3D12_TEXTURE_ADDRESS_MODE_BORDER: return MTLSamplerAddressModeClampToBorderColor;
        case D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE: return MTLSamplerAddressModeMirrorClampToEdge;
        default: return MTLSamplerAddressModeClampToEdge;
    }
}

MTLVertexFormat MtlVertexFormat(uint32_t dxgi) {
    switch (dxgi) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return MTLVertexFormatFloat4;
        case DXGI_FORMAT_R32G32B32_FLOAT: return MTLVertexFormatFloat3;
        case DXGI_FORMAT_R32G32_FLOAT: return MTLVertexFormatFloat2;
        case DXGI_FORMAT_R32_FLOAT: return MTLVertexFormatFloat;
        case DXGI_FORMAT_R32_UINT: return MTLVertexFormatUInt;
        case DXGI_FORMAT_R16G16B16A16_SNORM: return MTLVertexFormatShort4Normalized;
        case DXGI_FORMAT_R16G16B16A16_SINT: return MTLVertexFormatShort4;
        case DXGI_FORMAT_R16G16_SNORM: return MTLVertexFormatShort2Normalized;
        case DXGI_FORMAT_R16G16_SINT: return MTLVertexFormatShort2;
        case DXGI_FORMAT_R16_SNORM: return MTLVertexFormatShortNormalized;
        case DXGI_FORMAT_R16_SINT: return MTLVertexFormatShort;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return MTLVertexFormatUChar4Normalized;
        case DXGI_FORMAT_R8G8_UNORM: return MTLVertexFormatUChar2Normalized;
        case DXGI_FORMAT_R8_UNORM: return MTLVertexFormatUCharNormalized;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return MTLVertexFormatUChar4Normalized_BGRA;
        default: return MTLVertexFormatInvalid;
    }
}

MTLPixelFormat MtlTextureFormat(Host h) {
    switch (h) {
        case Host::BC1: return MTLPixelFormatBC1_RGBA;
        case Host::BC2: return MTLPixelFormatBC2_RGBA;
        case Host::BC3: return MTLPixelFormatBC3_RGBA;
        case Host::R32F: return MTLPixelFormatR32Float;
        default: return kBgra;
    }
}

uint32_t BlockBytes(MTLPixelFormat f) {
    switch (f) {
        case MTLPixelFormatBC1_RGBA: return 8;
        case MTLPixelFormatBC2_RGBA: case MTLPixelFormatBC3_RGBA: return 16;
        default: return 0;
    }
}

// a full screen triangle that writes a colour or a depth, for clears of part of a target
const char* kClearHlsl = R"(
cbuffer B : register(b1) { float4 colour; float4 depth; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 pscolour(float4 p : SV_Position) : SV_Target { return colour; }
float psdepth(float4 p : SV_Position) : SV_Depth { return depth.x; }
)";

// a console surface drawn into
struct Target {
    id<MTLTexture> res = nil;
    id<MTLTexture> fb = nil;  // copy read by draws that sample the target they render to
    bool depth = false;
    uint32_t gw = 0, gh = 0, w = 0, h = 0;
    uint32_t data = 0, bytes = 0;
    uint64_t epoch = 0, sig = 0;
    bool drawn = false;
    bool linear32 = true;
    bool swizzled32 = false;
    std::vector<uint8_t> read;
    id<MTLTexture> ms = nil;  // MSAA: the multisampled twin draws go to
    bool ms_dirty = false;    // the twin has pixels res has not
    bool ms_stale = false;    // res has pixels the twin has not
};

struct Tex {
    id<MTLTexture> res = nil;
    TextureDesc d;
    MTLPixelFormat fmt = MTLPixelFormatInvalid;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    uint64_t palette = 0;
    uint64_t faces = 0;
};

struct DrawTextures {
    TexInfo info[4];
    id<MTLTexture> tex[4] = {nil, nil, nil, nil};
    uint32_t levels[4] = {1, 1, 1, 1};
    bool replaced[4] = {};
    StageReads reads;
};

struct BlitOptions {
    bool gamma = false;
    bool alpha = false;
    uint32_t taps = 1;
    bool over = false;
    bool final = false;
};

bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++)
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    return false;
}

bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = SurfaceSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

std::vector<uint8_t> LatchCpuChanges(Target& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    }
    return px;
}

uint64_t TargetMapKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }
constexpr int kKeyWidthShift = 34;
constexpr int kKeyHeightShift = 48;

struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B;
    constexpr uint32_t kPaletteCommon = 0;
    constexpr uint32_t kPaletteData = 4;
    constexpr uint32_t kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.res && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

class MetalBackend final : public Backend {
public:
    ~MetalBackend() override {
        if (ui_) ImGui_ImplMetal_Shutdown();
    }

    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override { Sync(data, 0); }
    void Flush() override {
        Commit();
        DeliverReports(true);
    }
    void SetGamma(const uint8_t* ramp) override {
        std::memcpy(ramp_, ramp, sizeof(ramp_));
        ramp_dirty_ = true;
    }
    void ClearReport() override { counted_.clear(); }
    void GetReport(uint32_t report) override { reports_.push_back({report, counted_, serial_}); }

private:
    struct Slot {
        uint32_t index;
        float samples_per_pixel;
        uint64_t serial;  // the command buffer that counts it
    };
    struct Report {
        uint32_t va;
        std::vector<Slot> slots;
        uint64_t serial;
    };
    struct Chunk {
        id<MTLBuffer> buf = nil;
        uint32_t used = 0;
        uint64_t serial = 0;  // the command buffer last using it
    };
    struct Upload {
        id<MTLBuffer> buf;
        uint32_t offset;
        uint8_t* p;
    };
    struct Pipeline {
        id<MTLRenderPipelineState> state = nil;
        id<MTLDepthStencilState> depth = nil;
        MTLCullMode cull = MTLCullModeNone;
        MTLWinding winding = MTLWindingClockwise;
        MTLTriangleFillMode fill = MTLTriangleFillModeFill;
        float bias = 0, slope = 0;
        bool ok = false;
    };

    // command buffers, encoders, upload memory
    id<MTLCommandBuffer> Cmd();
    void Commit(bool wait = false);
    void EndEncoders();
    id<MTLRenderCommandEncoder> Render(id<MTLTexture> colour, id<MTLTexture> depth);
    id<MTLBlitCommandEncoder> BlitEncoder();
    Upload Allocate(uint32_t bytes, uint32_t align = 256);
    void ReclaimChunks();
    void UploadToTexture(id<MTLTexture> tex, uint32_t slice, uint32_t level, uint32_t w, uint32_t h, uint32_t depth,
                         const uint8_t* rows, uint32_t row_bytes, uint32_t n_rows);
    // targets
    uint64_t SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const;
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth, uint32_t gw = 0, uint32_t gh = 0);
    Target* DepthTarget(const State& s);
    bool CreateTarget(Target& t);
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    bool TargetStale(uint32_t data) const;
    bool BindTargets(const State& s);
    bool MsEligible(const Target& t) const { return msaa_ > 1 && (t.depth || (t.gw >= 320 && t.gh >= 240)); }
    bool EnsureMs(Target& t);
    void Settle(Target& t);   // the twin's pixels into res
    void Refresh(Target& t);  // res's pixels into the twin
    void Wrote(Target& t) {   // res written by other than a draw: the twin is behind
        if (t.ms) t.ms_stale = true, t.ms_dirty = false;
    }
    void DropMs(Target& t);
    void UploadTarget(Target& t, const Surface& s);
    id<MTLTexture> Shrink(Target& t, uint32_t w, uint32_t h);
    id<MTLTexture> PrepareCopySource(Target& t);
    bool ReadBack(id<MTLTexture> src, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
    id<MTLTexture> NewTexture(MTLPixelFormat fmt, uint32_t w, uint32_t h, MTLTextureUsage usage);
    // shaders, passes
    const Compiled* Compile(const std::string& src, const char* entry, bool vertex, uint64_t& hash, float point_size = 0);
    void FullScreen(id<MTLTexture> colour, id<MTLTexture> depth, const Compiled* vs, const Compiled* ps, const float* cb,
                    uint32_t cb_floats, id<MTLTexture> const* tex, uint32_t n_tex, bool over, int depth_mode,
                    const MTLViewport* vp = nullptr, const MTLScissorRect* sc = nullptr, uint32_t write_mask = 0xF,
                    uint32_t stencil_ref = 0, bool stencil = false);
    void Blit(id<MTLTexture> dst, id<MTLTexture> src, const float uvrect[4], const BlitOptions& o,
              const MTLViewport* vp = nullptr, const MTLScissorRect* sc = nullptr);
    bool DepthCopy(const State& s);
    void ClearRects(Target& t, const std::vector<RECT>& rs, uint32_t flags, const float colour[4], float z,
                    uint32_t stencil);
    void CopyByDraw(Target& src_t, Target& dst_t, uint32_t count, uint32_t src_rects, uint32_t dst_points);
    void CopyByRegion(Target& src_t, Target& dst_t, uint32_t count, uint32_t src_rects, uint32_t dst_points);
    // a draw's steps
    id<MTLTexture> BindTexture(const State& s, int stage, DrawTextures& stages);
    id<MTLTexture> BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, DrawTextures& stages);
    id<MTLTexture> BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                    DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi);
    void UploadTexture(Tex& tx, const TextureDesc& d, const uint8_t* p, const uint32_t* colours);
    id<MTLTexture> BindReplacement(const Replacement& r, const TextureDesc& d, uint32_t& levels);
    void CopyCubeTargets(Tex& tx, const TextureDesc& d);
    Pipeline* EnsurePipeline(const PsoKey& k, const Assembled& a, const Compiled* vs, const Compiled* ps, uint64_t vh,
                             uint64_t ph);
    id<MTLSamplerState> Sampler(const D3D12_SAMPLER_DESC& d);
    float SamplesPerPixel() const;
    void DeliverReports(bool wait);
    // present
    void UploadGammaRamp();
    id<MTLTexture> SmaaFrame(Target& t);  // Settings::smaa: the frame through SMAA's passes, nil when it cannot
    void RenderUi(id<MTLRenderCommandEncoder> enc, MTLRenderPassDescriptor* rpd);
    void ReleaseOldTextures();
    void CountFps();
    void ApplySettings();
    void RescaleTargets();

    id<MTLDevice> dev_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    CAMetalLayer* layer_ = nil;
    id<MTLCommandBuffer> cmd_ = nil;
    id<MTLRenderCommandEncoder> enc_ = nil;
    id<MTLBlitCommandEncoder> blit_ = nil;
    id<MTLTexture> enc_colour_ = nil, enc_depth_ = nil;
    dispatch_semaphore_t in_flight_ = nullptr;
    uint64_t serial_ = 1;                 // the command buffer being recorded
    std::atomic<uint64_t> completed_{0};  // the last one the GPU finished
    std::vector<Chunk> chunks_;           // upload memory: the current one first
    std::vector<id<MTLBuffer>> vis_free_;
    std::deque<std::pair<uint64_t, id<MTLBuffer>>> vis_busy_;
    id<MTLBuffer> vis_ = nil;             // the command buffer's visibility results
    uint32_t vis_used_ = 0;
    std::unordered_map<uint64_t, id<MTLBuffer>> vis_by_serial_;
    // fixed objects
    id<MTLSamplerState> pass_samplers_[4] = {nil, nil, nil, nil};
    std::unordered_map<uint64_t, id<MTLSamplerState>> samplers_;
    std::unordered_map<uint64_t, id<MTLDepthStencilState>> depth_states_;
    std::unordered_map<uint64_t, id<MTLRenderPipelineState>> pass_pipelines_;
    id<MTLTexture> lut_ = nil;
    id<MTLTexture> smaa_[3] = {nil, nil, nil};  // edges, weights, the result
    id<MTLTexture> smaa_area_ = nil, smaa_search_ = nil;
    // caches
    std::unordered_map<uint64_t, Compiled> shaders_;     // by source hash (point size in it)
    std::unordered_map<uint64_t, uint64_t> shader_src_;  // vertex and pixel keys to source hash
    std::unordered_map<uint64_t, Pipeline> pipelines_;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint32_t, uint64_t> target_used_;
    std::unordered_map<uint64_t, Tex> textures_;
    std::unordered_map<uint32_t, uint32_t> reloads_;
    std::unordered_map<uint64_t, id<MTLTexture>> shrinks_;  // cube faces' shrunk copies, by size
    std::vector<Slot> counted_;
    std::deque<Report> reports_;
    Assembled asm_;
    bool draw_2d_ = false;
    Target* bound_rt_ = nullptr;
    Target* bound_ds_ = nullptr;
    bool bound_ms_ = false;
    uint32_t msaa_ = 1;
    Target* draw_rt_ = nullptr;
    Target* draw_ds_ = nullptr;
    uint32_t guest_w_ = 640, guest_h_ = 480;
    float scale_ = 1;
    float aspect_ = 0;
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = false;
    bool gamma_ = false;
    bool ui_ = false;
    uint64_t presents_ = 0;
    double fps_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
    // XBE_PERF: where a frame's time goes, logged every 10 s
    struct Perf {
        uint64_t readbacks = 0, report_waits = 0, commits = 0, draws = 0, passes = 0;
        double readback_ms = 0, drawable_ms = 0, wait_ms = 0;
    } perf_;
    std::chrono::steady_clock::time_point perf_t_ = std::chrono::steady_clock::now();
};

// ---- device

bool MetalBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    aspect_ = app::FrameAspect();
    scale_ = app::RenderScale(guest_h_);
    layer_ = (__bridge CAMetalLayer*)app::MetalLayer();
    dev_ = layer_ && layer_.device ? layer_.device : MTLCreateSystemDefaultDevice();
    if (!dev_) {
        LOGE("metal: no device");
        return false;
    }
    queue_ = [dev_ newCommandQueueWithMaxCommandBufferCount:16];
    if (layer_) {
        layer_.device = dev_;
        layer_.pixelFormat = kBgra;
        layer_.framebufferOnly = NO;
        layer_.maximumDrawableCount = 3;
        layer_.displaySyncEnabled = app::Get().vsync;
    }
    in_flight_ = dispatch_semaphore_create(kInFlight);
    for (int i = 0; i < 4; i++) {
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = i == 0 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.mipFilter = i == 0 ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
        sd.sAddressMode = sd.tAddressMode = sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
        pass_samplers_[i] = [dev_ newSamplerStateWithDescriptor:sd];
    }
    lut_ = NewTexture(kBgra, 256, 1, MTLTextureUsageShaderRead);
    g_sample_bias = true;
    ui_ = ImGui_ImplMetal_Init(dev_);
    LOGR("metal: %s, %ux%u guest, render scale %.3f", [[dev_ name] UTF8String], width, height, scale_);
    return true;
}

id<MTLTexture> MetalBackend::NewTexture(MTLPixelFormat fmt, uint32_t w, uint32_t h, MTLTextureUsage usage) {
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
    td.usage = usage;
    td.storageMode = MTLStorageModePrivate;
    return [dev_ newTextureWithDescriptor:td];
}

// ---- command buffers and encoders

id<MTLCommandBuffer> MetalBackend::Cmd() {
    if (cmd_) return cmd_;
    dispatch_semaphore_wait(in_flight_, DISPATCH_TIME_FOREVER);
    cmd_ = [queue_ commandBuffer];
    const uint64_t serial = serial_;
    dispatch_semaphore_t sem = in_flight_;
    std::atomic<uint64_t>* done = &completed_;
    [cmd_ addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        if (cb.error) LOG_FIRST(8, "metal: command buffer %llu failed: %s", (unsigned long long)serial,
                                [[cb.error localizedDescription] UTF8String]);
        uint64_t prev = done->load();
        while (prev < serial && !done->compare_exchange_weak(prev, serial)) {}
        dispatch_semaphore_signal(sem);
    }];
    // the visibility results this command buffer's counted draws write
    while (!vis_busy_.empty() && vis_busy_.front().first <= completed_.load()) {
        vis_free_.push_back(vis_busy_.front().second);
        vis_busy_.pop_front();
    }
    if (!vis_free_.empty()) {
        vis_ = vis_free_.back();
        vis_free_.pop_back();
    } else {
        vis_ = [dev_ newBufferWithLength:kVisibilitySlots * 8 options:MTLResourceStorageModeShared];
    }
    vis_used_ = 0;
    return cmd_;
}

void MetalBackend::EndEncoders() {
    if (enc_) [enc_ endEncoding];
    if (blit_) [blit_ endEncoding];
    enc_ = nil;
    blit_ = nil;
    enc_colour_ = enc_depth_ = nil;
}

void MetalBackend::Commit(bool wait) {
    if (!cmd_) return;
    perf_.commits++;
    EndEncoders();
    id<MTLCommandBuffer> cb = cmd_;
    vis_busy_.push_back({serial_, vis_});
    vis_by_serial_[serial_] = vis_;
    for (Chunk& c : chunks_)
        if (c.used && c.serial == 0) c.serial = serial_;
    [cb commit];
    cmd_ = nil;
    vis_ = nil;
    serial_++;
    if (wait) {
        auto t0 = std::chrono::steady_clock::now();
        [cb waitUntilCompleted];
        perf_.wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
}

id<MTLRenderCommandEncoder> MetalBackend::Render(id<MTLTexture> colour, id<MTLTexture> depth) {
    if (enc_ && enc_colour_ == colour && enc_depth_ == depth) return enc_;
    if (vis_used_ >= kVisibilitySlots) Commit();
    Cmd();
    EndEncoders();
    MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    if (colour) {
        rpd.colorAttachments[0].texture = colour;
        rpd.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    if (depth) {
        rpd.depthAttachment.texture = depth;
        rpd.depthAttachment.loadAction = MTLLoadActionLoad;
        rpd.depthAttachment.storeAction = MTLStoreActionStore;
        rpd.stencilAttachment.texture = depth;
        rpd.stencilAttachment.loadAction = MTLLoadActionLoad;
        rpd.stencilAttachment.storeAction = MTLStoreActionStore;
    }
    if (!colour && !depth) return nil;
    rpd.visibilityResultBuffer = vis_;
    enc_ = [cmd_ renderCommandEncoderWithDescriptor:rpd];
    enc_colour_ = colour;
    enc_depth_ = depth;
    return enc_;
}

id<MTLBlitCommandEncoder> MetalBackend::BlitEncoder() {
    if (blit_) return blit_;
    Cmd();
    EndEncoders();
    blit_ = [cmd_ blitCommandEncoder];
    return blit_;
}

void MetalBackend::ReclaimChunks() {
    uint64_t done = completed_.load();
    for (Chunk& c : chunks_)
        if (c.serial && c.serial <= done) c.used = 0, c.serial = 0;
}

// bytes the GPU reads in the command buffer being recorded
MetalBackend::Upload MetalBackend::Allocate(uint32_t bytes, uint32_t align) {
    Cmd();
    for (int pass = 0; pass < 2; pass++) {
        for (Chunk& c : chunks_) {
            if (c.serial) continue;  // a committed command buffer still reads it
            uint32_t o = (c.used + align - 1) & ~(align - 1);
            if (o + bytes <= uint32_t([c.buf length])) {
                c.used = o + bytes;
                return {c.buf, o, static_cast<uint8_t*>([c.buf contents]) + o};
            }
        }
        if (pass == 0) ReclaimChunks();
    }
    Chunk c;
    c.buf = [dev_ newBufferWithLength:std::max(kChunkSize, (bytes + 0xFFFFu) & ~0xFFFFu)
                              options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
    c.used = bytes;
    chunks_.push_back(c);
    return {c.buf, 0, static_cast<uint8_t*>([c.buf contents])};
}

// rows into a texture's level, in order with the GPU's other work on it
void MetalBackend::UploadToTexture(id<MTLTexture> tex, uint32_t slice, uint32_t level, uint32_t w, uint32_t h,
                                   uint32_t depth, const uint8_t* rows, uint32_t row_bytes, uint32_t n_rows) {
    uint32_t bytes = row_bytes * n_rows * depth;
    Upload u = Allocate(bytes, 256);
    std::memcpy(u.p, rows, bytes);
    [BlitEncoder() copyFromBuffer:u.buf
                     sourceOffset:u.offset
                sourceBytesPerRow:row_bytes
              sourceBytesPerImage:row_bytes * n_rows
                       sourceSize:MTLSizeMake(w, h, depth)
                        toTexture:tex
                 destinationSlice:slice
                 destinationLevel:level
                destinationOrigin:MTLOriginMake(0, 0, 0)];
}

// ---- shaders and passes

const Compiled* MetalBackend::Compile(const std::string& src, const char* entry, bool vertex, uint64_t& hash, float point_size) {
    uint64_t seed = Hash(entry, std::strlen(entry), vertex ? 0x5653ull : 0x5053ull);
    seed = Hash(&point_size, sizeof(point_size), seed);
    hash = Hash(src.data(), src.size(), seed);
    auto it = shaders_.find(hash);
    if (it != shaders_.end()) return it->second.fn ? &it->second : nullptr;
    Compiled& c = shaders_[hash];
    std::string error, msl;
    {
        MslCache& mc = Msls();
        std::lock_guard lock(mc.m);
        if (!mc.loaded) LoadMslCache(mc);
        if (auto it = mc.entries.find(hash); it != mc.entries.end()) {
            msl = it->second.first;
            std::memcpy(c.attr_reg, it->second.second.data(), 16);
        }
    }
    const bool translated = msl.empty();
    if (translated) {
        g_shaders_compiled++;  // the frames line's count: shaders translated, not those the disk cache had
        msl = HlslToMsl(src, entry, vertex, point_size, c.attr_reg, error);
    }
    if (msl.empty()) {
        LOGE("metal: %s %s did not translate: %s", vertex ? "vertex" : "pixel", entry, error.c_str());
        LOGE("%s", src.c_str());
        return nullptr;
    }
    MTLCompileOptions* opts = [MTLCompileOptions new];
    opts.mathMode = MTLMathModeSafe;  // EQUAL depth passes need bit exact positions
    opts.languageVersion = MTLLanguageVersion2_3;
    NSError* err = nil;
    id<MTLLibrary> lib = [dev_ newLibraryWithSource:[NSString stringWithUTF8String:msl.c_str()] options:opts error:&err];
    if (!lib) {
        LOGE("metal: %s %s did not compile: %s", vertex ? "vertex" : "pixel", entry,
             err ? [[err localizedDescription] UTF8String] : "?");
        LOGE("%s", msl.c_str());
        return nullptr;
    }
    NSArray<NSString*>* names = [lib functionNames];
    c.fn = names.count ? [lib newFunctionWithName:names[0]] : nil;
    if (c.fn && translated) {
        MslCache& mc = Msls();
        std::lock_guard lock(mc.m);
        std::array<int8_t, 16> regs;
        std::memcpy(regs.data(), c.attr_reg, 16);
        if (mc.entries.emplace(hash, std::make_pair(msl, regs)).second && mc.out) {
            uint32_t n = uint32_t(msl.size());
            std::fwrite(&hash, 8, 1, mc.out);
            std::fwrite(regs.data(), 16, 1, mc.out);
            std::fwrite(&n, 4, 1, mc.out);
            std::fwrite(msl.data(), 1, n, mc.out);
            std::fflush(mc.out);
        }
    }
    return c.fn ? &c : nullptr;
}

// the full screen triangle into colour or depth, cb at buffers 0 and 1, textures from 0, the pass samplers.
// depth_mode: 0 no depth test, 1 depth always passing and written
void MetalBackend::FullScreen(id<MTLTexture> colour, id<MTLTexture> depth, const Compiled* vs, const Compiled* ps,
                              const float* cb, uint32_t cb_floats, id<MTLTexture> const* tex, uint32_t n_tex, bool over,
                              int depth_mode, const MTLViewport* vp, const MTLScissorRect* sc, uint32_t write_mask,
                              uint32_t stencil_ref, bool stencil) {
    if (!vs || !ps) return;
    id<MTLRenderCommandEncoder> enc = Render(colour, depth);
    if (!enc) return;
    struct {
        const void* vs;
        const void* ps;
        uint64_t colour, depth;
        uint32_t over, mask, samples;
    } key{(__bridge const void*)vs->fn, (__bridge const void*)ps->fn, colour ? uint64_t(colour.pixelFormat) : 0,
          depth ? uint64_t(depth.pixelFormat) : 0, over, write_mask, uint32_t((colour ? colour : depth).sampleCount)};
    uint64_t h = Hash(&key, sizeof(key));
    __strong id<MTLRenderPipelineState>& pso = pass_pipelines_[h];
    if (!pso) {
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = vs->fn;
        pd.fragmentFunction = ps->fn;
        if (colour) {
            auto* ca = pd.colorAttachments[0];
            ca.pixelFormat = colour.pixelFormat;
            ca.writeMask = ((write_mask & 1) ? MTLColorWriteMaskRed : 0) | ((write_mask & 2) ? MTLColorWriteMaskGreen : 0) |
                           ((write_mask & 4) ? MTLColorWriteMaskBlue : 0) | ((write_mask & 8) ? MTLColorWriteMaskAlpha : 0);
            if (over) {  // premultiplied colour, the target keeps its alpha
                ca.blendingEnabled = YES;
                ca.sourceRGBBlendFactor = MTLBlendFactorOne;
                ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                ca.sourceAlphaBlendFactor = MTLBlendFactorZero;
                ca.destinationAlphaBlendFactor = MTLBlendFactorOne;
            }
        }
        if (depth) pd.depthAttachmentPixelFormat = pd.stencilAttachmentPixelFormat = depth.pixelFormat;
        pd.rasterSampleCount = (colour ? colour : depth).sampleCount;
        NSError* err = nil;
        pso = [dev_ newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!pso) {
            LOGE("metal: pass pipeline failed: %s", err ? [[err localizedDescription] UTF8String] : "?");
            pass_pipelines_.erase(h);
            return;
        }
    }
    uint64_t dkey = 0xF00000000ull | uint64_t(depth_mode) << 1 | (stencil ? 1 : 0);
    __strong id<MTLDepthStencilState>& dss = depth_states_[dkey];
    if (!dss) {
        MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        dd.depthWriteEnabled = depth_mode == 1;
        if (stencil) {
            MTLStencilDescriptor* st = [MTLStencilDescriptor new];
            st.stencilCompareFunction = MTLCompareFunctionAlways;
            st.stencilFailureOperation = st.depthFailureOperation = st.depthStencilPassOperation = MTLStencilOperationReplace;
            st.readMask = st.writeMask = 0xFF;
            dd.frontFaceStencil = dd.backFaceStencil = st;
        }
        dss = [dev_ newDepthStencilStateWithDescriptor:dd];
    }
    uint32_t w = uint32_t((colour ? colour : depth).width), hgt = uint32_t((colour ? colour : depth).height);
    [enc setRenderPipelineState:pso];
    [enc setDepthStencilState:dss];
    [enc setStencilReferenceValue:stencil_ref];
    [enc setCullMode:MTLCullModeNone];
    [enc setTriangleFillMode:MTLTriangleFillModeFill];
    [enc setDepthBias:0 slopeScale:0 clamp:0];
    [enc setViewport:vp ? *vp : MTLViewport{0, 0, double(w), double(hgt), 0, 1}];
    [enc setScissorRect:sc ? *sc : MTLScissorRect{0, 0, w, hgt}];
    if (cb) {
        [enc setVertexBytes:cb length:cb_floats * 4 atIndex:0];
        [enc setVertexBytes:cb length:cb_floats * 4 atIndex:1];
        [enc setFragmentBytes:cb length:cb_floats * 4 atIndex:0];
        [enc setFragmentBytes:cb length:cb_floats * 4 atIndex:1];
    }
    for (uint32_t i = 0; i < n_tex; i++) [enc setFragmentTexture:tex[i] atIndex:i];
    [enc setFragmentSamplerStates:pass_samplers_ withRange:NSMakeRange(0, 4)];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

void MetalBackend::Blit(id<MTLTexture> dst, id<MTLTexture> src, const float uvrect[4], const BlitOptions& o,
                        const MTLViewport* vp, const MTLScissorRect* sc) {
    uint64_t h;
    const app::Settings& st = app::Get();
    const bool final = o.final && (st.fxaa || st.sharpen || st.scale_filter);
    const Compiled* vs = Compile(kBlitHlsl, "vs", true, h);
    const Compiled* ps = Compile(kBlitHlsl, final ? "psfinal" : "psmain", false, h);
    if (!ps && final) ps = Compile(kBlitHlsl, "psmain", false, h);
    float c[8] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3], o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f,
                  float(o.taps), 1.0f};
    if (final) c[5] = float((st.fxaa && !st.smaa ? 1 : 0) | (st.scale_filter ? 2 : 0)), c[6] = float(std::min<uint32_t>(st.sharpen, 100)) / 100.0f;
    id<MTLTexture> tex[2] = {src, lut_};
    FullScreen(dst, nil, vs, ps, c, 8, tex, 2, o.over, 0, vp, sc);
}

// a depth buffer drawn as colour into another depth buffer's memory: copied as depth
bool MetalBackend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube || d.depth != 1)
        return false;
    uint32_t gw, gh;
    SurfaceSize(s.rt, gw, gh);
    auto di = targets_.find(SizedTargetKey(mem::VirtualFromPhysical(s.rt.data), true, gw, gh));
    auto si = targets_.find(SizedTargetKey(d.data, true, d.width, d.height));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;
    Target& dst = di->second;
    Target& src = si->second;
    uint64_t h;
    const Compiled* vs = Compile(kDepthHlsl, "vs", true, h);
    const Compiled* ps = Compile(kDepthHlsl, "psmain", false, h);
    if (!vs || !ps) return false;
    Settle(src);
    Settle(dst);
    Wrote(dst);
    id<MTLTexture> tex = src.res;
    FullScreen(nil, dst.res, vs, ps, nullptr, 0, &tex, 1, false, 1);
    dst.drawn = true;
    return true;
}

// ---- MSAA twins

bool MetalBackend::EnsureMs(Target& t) {
    if (t.ms) return true;
    if (![dev_ supportsTextureSampleCount:msaa_]) return false;
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.depth ? kDepthFormat : kBgra
                                                                                 width:t.w
                                                                                height:t.h
                                                                             mipmapped:NO];
    td.textureType = MTLTextureType2DMultisample;
    td.sampleCount = msaa_;
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    t.ms = [dev_ newTextureWithDescriptor:td];
    if (!t.ms) {
        LOG_FIRST(4, "metal: MSAA twin of %08X %ux%u could not be made", t.data, t.w, t.h);
        return false;
    }
    t.ms_dirty = false;
    t.ms_stale = true;  // filled from res before its first draw
    LOG_FIRST(8, "metal: MSAA %ux twin for %s %08X %ux%u", msaa_, t.depth ? "depth" : "target", t.data, t.w, t.h);
    return true;
}

// a pass that only resolves: the twin kept, res given its resolve (depth and stencil from sample 0)
void MetalBackend::Settle(Target& t) {
    if (!t.ms || !t.ms_dirty) return;
    t.ms_dirty = false;
    Cmd();
    EndEncoders();
    MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    if (t.depth) {
        rpd.depthAttachment.texture = rpd.stencilAttachment.texture = t.ms;
        rpd.depthAttachment.resolveTexture = rpd.stencilAttachment.resolveTexture = t.res;
        rpd.depthAttachment.loadAction = rpd.stencilAttachment.loadAction = MTLLoadActionLoad;
        rpd.depthAttachment.storeAction = rpd.stencilAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
        rpd.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
        rpd.stencilAttachment.stencilResolveFilter = MTLMultisampleStencilResolveFilterSample0;
    } else {
        rpd.colorAttachments[0].texture = t.ms;
        rpd.colorAttachments[0].resolveTexture = t.res;
        rpd.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rpd.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
    }
    [[cmd_ renderCommandEncoderWithDescriptor:rpd] endEncoding];
}

void MetalBackend::Refresh(Target& t) {
    if (!t.ms || !t.ms_stale) return;
    t.ms_stale = false;
    uint64_t h;
    if (!t.depth) {
        float uv[4] = {0, 0, 1, 1};
        Blit(t.ms, t.res, uv, {.alpha = true});
        return;
    }
    const Compiled* vs = Compile(kDepthHlsl, "vs", true, h);
    const Compiled* ps = Compile(kDepthHlsl, "psmain", false, h);
    id<MTLTexture> tex = t.res;
    FullScreen(nil, t.ms, vs, ps, nullptr, 0, &tex, 1, false, 1);
}

void MetalBackend::DropMs(Target& t) {
    if (!t.ms) return;
    Settle(t);
    if (enc_colour_ == t.ms || enc_depth_ == t.ms) EndEncoders();
    t.ms = nil;
    t.ms_dirty = t.ms_stale = false;
}

// ---- targets

uint64_t MetalBackend::SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const {
    uint64_t key = TargetMapKey(data, depth);
    bool small_depth = depth && (gw != guest_w_ || gh != guest_h_);
    if (!small_depth) return key;
    return key | uint64_t(gw) << kKeyWidthShift | uint64_t(gh) << kKeyHeightShift;
}

Target* MetalBackend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data);
    uint32_t gw, gh;
    SurfaceSize(s, gw, gh);
    auto it = targets_.find(SizedTargetKey(data, depth, gw, gh));
    if (it == targets_.end() || it->second.gw != gw || it->second.gh != gh) return nullptr;
    target_used_[data] = presents_;
    return &it->second;
}

bool MetalBackend::CreateTarget(Target& t) {
    t.w = std::max(1u, uint32_t(std::lround(t.gw * scale_ * XStretch(aspect_, guest_h_, t.gw, t.gh))));
    t.h = std::max(1u, uint32_t(std::lround(t.gh * scale_)));
    t.res = NewTexture(t.depth ? kDepthFormat : kBgra, t.w, t.h, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    if (!t.res) return false;
    // a new target starts cleared: black, or depth 1 and stencil 0
    Cmd();
    EndEncoders();
    MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    if (t.depth) {
        rpd.depthAttachment.texture = rpd.stencilAttachment.texture = t.res;
        rpd.depthAttachment.loadAction = rpd.stencilAttachment.loadAction = MTLLoadActionClear;
        rpd.depthAttachment.storeAction = rpd.stencilAttachment.storeAction = MTLStoreActionStore;
        rpd.depthAttachment.clearDepth = 1;
        rpd.stencilAttachment.clearStencil = 0;
    } else {
        rpd.colorAttachments[0].texture = t.res;
        rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
        rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
        rpd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    }
    [[cmd_ renderCommandEncoderWithDescriptor:rpd] endEncoding];
    return true;
}

void MetalBackend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    if (bound_rt_ == &it->second || bound_ds_ == &it->second) bound_rt_ = bound_ds_ = nullptr;
    if (enc_colour_ == it->second.res || enc_depth_ == it->second.res || (it->second.ms && (enc_colour_ == it->second.ms ||
                                                                                          enc_depth_ == it->second.ms)))
        EndEncoders();
    targets_.erase(it);
}

Target* MetalBackend::EnsureTarget(const Surface& s, bool depth, uint32_t gw, uint32_t gh) {
    if (!s.header) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data);
    uint32_t sw, sh;
    SurfaceSize(s, sw, sh);
    if (gw && gh) sw = gw, sh = gh;
    uint64_t key = SizedTargetKey(data, depth, sw, sh);
    target_used_[data] = presents_;
    auto it = targets_.find(key);
    if (it != targets_.end()) {
        if (it->second.gw == sw && it->second.gh == sh) return &it->second;
        ReleaseTarget(it);
    }
    Target t;
    t.depth = depth;
    t.gw = sw;
    t.gh = sh;
    if (!CreateTarget(t)) {
        LOG_FIRST(8, "metal: target %08X %ux%u could not be made", data, sw, sh);
        return nullptr;
    }
    t.data = data;
    TextureDesc d;
    t.linear32 = depth || (Describe(s.header, d) && d.linear && Format(d.format).bits == 32);
    t.swizzled32 = !depth && !t.linear32 && Describe(s.header, d) && Format(d.format).swizzled &&
                   !Format(d.format).compressed && Format(d.format).bits == 32;
    t.bytes = depth ? sw * sh * 4 : GuestPitch(sw) * sh;
    t.epoch = DirtyEpoch(data);
    t.sig = SurfaceSig(data, t.bytes);
    return &(targets_[key] = std::move(t));
}

Target* MetalBackend::DepthTarget(const State& s) {
    uint32_t rw = 0, rh = 0, zw = 0, zh = 0;
    if (s.rt.header && s.zs.header) {
        SurfaceSize(s.rt, rw, rh);
        SurfaceSize(s.zs, zw, zh);
        if (rw <= zw && rh <= zh && (rw != zw || rh != zh)) return EnsureTarget(s.zs, true, rw, rh);
    }
    return EnsureTarget(s.zs, true);
}

bool MetalBackend::TargetStale(uint32_t data) const {
    auto it = target_used_.find(data);
    uint64_t used = it == target_used_.end() ? 0 : it->second;
    return presents_ - used > kStaleFrames;
}

// the draw's targets (d3d11.cpp BindTargets); the render pass opens when the draw is recorded
bool MetalBackend::BindTargets(const State& s) {
    bool depth = (s.rs[kRsZEnable] || s.rs[kRsStencilEnable]) && !DepthOnlySampled(s);
    bool shared = s.rt.header && s.zs.header && s.rt.data == s.zs.data;
    bool colour = SimpleState(s, kMColorWriteEnable) != 0;
    Target* rt = shared && depth && !colour ? nullptr : EnsureTarget(s.rt, false);
    Target* ds = depth && !(shared && colour) ? DepthTarget(s) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return false;
    if (rt && LatchCpuWrite(*rt)) {
        if (++reloads_[rt->data] <= 3)
            LOGR("metal: target %08X %ux%u reloaded from guest memory before drawing (frame %llu)", rt->data, rt->gw,
                 rt->gh, (unsigned long long)presents_);
        UploadTarget(*rt, s.rt);
    }
    // MSAA: an eligible colour target and its depth draw into their twins; otherwise into res, with the twins'
    // pixels settled into it first and the twins behind after
    bool ms = rt && MsEligible(*rt) && EnsureMs(*rt) && (!ds || EnsureMs(*ds));
    for (Target* t : {rt, ds}) {
        if (!t) continue;
        if (ms) {
            Refresh(*t);
            t->ms_dirty = true;
        } else {
            Settle(*t);
            Wrote(*t);
        }
        t->drawn = true;
    }
    bound_rt_ = rt;
    bound_ds_ = ds;
    bound_ms_ = ms;
    return true;
}

void MetalBackend::UploadTarget(Target& t, const Surface& s) {
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    if (px.size() < size_t(pitch) * d.height) px.resize(size_t(pitch) * d.height);
    id<MTLTexture> tex = NewTexture(kBgra, d.width, d.height, MTLTextureUsageShaderRead);
    if (!tex) return;
    Settle(t);
    Wrote(t);
    UploadToTexture(tex, 0, 0, d.width, d.height, 1, px.data(), pitch, d.height);
    float uv[4] = {0, 0, 1, 1};
    Blit(t.res, tex, uv, {.alpha = true, .over = over});
    t.epoch = DirtyEpoch(t.data);
    t.sig = SurfaceSig(t.data, t.bytes);
}

id<MTLTexture> MetalBackend::Shrink(Target& t, uint32_t w, uint32_t h) {
    id<MTLTexture> down = NewTexture(kBgra, w, h, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    if (!down) return nil;
    float uv[4] = {0, 0, 1, 1};
    Blit(down, t.res, uv, {.alpha = true});
    return down;
}

id<MTLTexture> MetalBackend::PrepareCopySource(Target& t) {
    Settle(t);
    if (t.w != t.gw || t.h != t.gh) return Shrink(t, t.gw, t.gh);
    return t.res;
}

// a BGRA texture to the CPU, rows packed; waits for the GPU
bool MetalBackend::ReadBack(id<MTLTexture> src, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    perf_.readbacks++;
    id<MTLBuffer> buf = [dev_ newBufferWithLength:size_t(w) * h * 4 options:MTLResourceStorageModeShared];
    if (!buf) return false;
    [BlitEncoder() copyFromTexture:src
                       sourceSlice:0
                       sourceLevel:0
                      sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake(w, h, 1)
                          toBuffer:buf
                 destinationOffset:0
            destinationBytesPerRow:w * 4
          destinationBytesPerImage:w * h * 4];
    Commit(true);
    out.assign(static_cast<uint8_t*>([buf contents]), static_cast<uint8_t*>([buf contents]) + size_t(w) * h * 4);
    return true;
}

void MetalBackend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn) return;
    if (TargetStale(data)) {
        if (it->second.bytes > kStaleReadMax || SurfaceSig(it->second.data, it->second.bytes) != it->second.sig) return;
        LOG_FIRST(8, "metal: read-back of %08X (%ux%u), unused for a while but its memory unchanged", data,
                  it->second.gw, it->second.gh);
    }
    Target& t = it->second;
    if (!t.linear32 && !t.swizzled32) {
        LOG_FIRST(8, "metal: read-back of %08X skipped, not a 32 bit surface", data);
        return;
    }
    id<MTLTexture> src = PrepareCopySource(t);
    std::vector<uint8_t> px;
    if (!src || !ReadBack(src, t.gw, t.gh, px)) return;
    DeliverReports(false);
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    if (t.swizzled32) {
        for (uint32_t y = 0; y < t.gh; y++)
            for (uint32_t x = 0; x < t.gw; x++)
                std::memcpy(guest + size_t(SwizzledOffset(x, y, t.gw, t.gh)) * 4, &px[(size_t(y) * t.gw + x) * 4], 4);
    } else {
        for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    }
    t.read = std::move(px);
    t.drawn = false;
    t.sig = SurfaceSig(t.data, t.bytes);
}

// ---- clears

void MetalBackend::ClearRects(Target& t, const std::vector<RECT>& rs, uint32_t flags, const float colour[4], float z,
                              uint32_t stencil) {
    uint64_t h;
    const Compiled* vs = Compile(kClearHlsl, "vs", true, h);
    const Compiled* ps = Compile(kClearHlsl, t.depth ? "psdepth" : "pscolour", false, h);
    float cb[8] = {colour[0], colour[1], colour[2], colour[3], z, 0, 0, 0};
    uint32_t mask = t.depth ? 0xF : (flags >> 4) & 0xF;
    for (const RECT& r : rs) {
        if (r.right <= r.left || r.bottom <= r.top) continue;
        MTLScissorRect sc{NSUInteger(r.left), NSUInteger(r.top), NSUInteger(r.right - r.left), NSUInteger(r.bottom - r.top)};
        // depth: written only when the clear asks for it (depth_mode 1 writes; stencil replaced when asked)
        FullScreen(t.depth ? nil : t.res, t.depth ? t.res : nil, vs, ps, cb, 8, nullptr, 0, false,
                   t.depth && (flags & kClearZ) ? 1 : 0, nullptr, &sc, mask, stencil & 0xFF,
                   t.depth && (flags & kClearStencil));
    }
}

void MetalBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                         uint32_t stencil) {
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? DepthTarget(s) : nullptr;
    std::vector<RECT> rs;
    float col[4];
    Color(color, col);
    for (Target* t : {rt, ds}) {
        if (!t) continue;
        rs.clear();
        if (count && rects) {
            for (uint32_t i = 0; i < count; i++) {
                int32_t r[4];
                std::memcpy(r, rects + size_t(i) * sizeof(r), sizeof(r));
                rs.push_back(ScaledRect(*t, r[0], r[1], r[2], r[3]));
            }
        } else {
            const auto& v = s.viewport;
            rs.push_back(ScaledRect(*t, int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h)));
        }
        bool whole = (t->depth || (flags & kClearTarget) == kClearTarget) && rs.size() == 1 && rs[0].left == 0 &&
                     rs[0].top == 0 && rs[0].right == LONG(t->w) && rs[0].bottom == LONG(t->h);
        if (!t->depth && LatchCpuWrite(*t)) UploadTarget(*t, s.rt);
        // a whole clear: a pass that only clears, res and the twin alike; part of one goes to res, the twin refreshed
        // later
        bool full = whole && (!t->depth || (flags & (kClearZ | kClearStencil)) == (kClearZ | kClearStencil));
        if (!full) Settle(*t);
        for (id<MTLTexture> tex : {t->res, full ? t->ms : nil}) {
            if (!tex) continue;
            if (!full) {
                ClearRects(*t, rs, flags, col, std::clamp(z, 0.0f, 1.0f), stencil);
                Wrote(*t);
                continue;
            }
            Cmd();
            EndEncoders();
            MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
            if (t->depth) {
                rpd.depthAttachment.texture = rpd.stencilAttachment.texture = tex;
                rpd.depthAttachment.loadAction = rpd.stencilAttachment.loadAction = MTLLoadActionClear;
                rpd.depthAttachment.storeAction = rpd.stencilAttachment.storeAction = MTLStoreActionStore;
                rpd.depthAttachment.clearDepth = std::clamp(z, 0.0f, 1.0f);
                rpd.stencilAttachment.clearStencil = stencil & 0xFF;
            } else {
                rpd.colorAttachments[0].texture = tex;
                rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
                rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
                rpd.colorAttachments[0].clearColor = MTLClearColorMake(col[0], col[1], col[2], col[3]);
            }
            [[cmd_ renderCommandEncoderWithDescriptor:rpd] endEncoding];
        }
        if (full && t->ms) t->ms_dirty = t->ms_stale = false;
        t->drawn = true;
    }
}

// ---- copies

void MetalBackend::CopyByDraw(Target& src_t, Target& dst_t, uint32_t count, uint32_t src_rects, uint32_t dst_points) {
    Settle(dst_t);
    Wrote(dst_t);
    float src_scale = float(src_t.w) / src_t.gw;
    float dst_scale = float(dst_t.w) / dst_t.gw;
    float dst_scale_y = float(dst_t.h) / dst_t.gh;
    bool listed = count && src_rects;
    uint32_t n = listed ? count : 1;
    for (uint32_t i = 0; i < n; i++) {
        float whole_w = float(std::min(src_t.gw, dst_t.gw));
        float whole_h = float(std::min(src_t.gh, dst_t.gh));
        DrawnRect r{.x0 = 0, .y0 = 0, .x1 = whole_w, .y1 = whole_h};
        if (listed) r = ReadDrawnRect(src_rects, dst_points, i);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        MTLViewport vp{r.dx * dst_scale, r.dy * dst_scale_y, (r.x1 - r.x0) * dst_scale, (r.y1 - r.y0) * dst_scale_y, 0, 1};
        LONG x0 = std::max<LONG>(LONG(vp.originX), 0), y0 = std::max<LONG>(LONG(vp.originY), 0);
        LONG right = std::min(LONG(vp.originX + vp.width), LONG(dst_t.w));
        LONG bottom = std::min(LONG(vp.originY + vp.height), LONG(dst_t.h));
        if (right <= x0 || bottom <= y0) continue;
        MTLScissorRect sc{NSUInteger(x0), NSUInteger(y0), NSUInteger(right - x0), NSUInteger(bottom - y0)};
        float uv[4] = {r.x0 / src_t.gw, r.y0 / src_t.gh, (r.x1 - r.x0) / src_t.gw, (r.y1 - r.y0) / src_t.gh};
        uint32_t taps = uint32_t(std::ceil(std::max(src_scale / dst_scale, 1.0f) - kTapsSlack));
        Blit(dst_t.res, src_t.res, uv, {.alpha = true, .taps = taps}, &vp, &sc);
    }
    dst_t.drawn = true;
}

void MetalBackend::CopyByRegion(Target& src_t, Target& dst_t, uint32_t count, uint32_t src_rects, uint32_t dst_points) {
    Settle(dst_t);
    Wrote(dst_t);
    float dst_scale = float(dst_t.w) / dst_t.gw;
    float dst_scale_y = float(dst_t.h) / dst_t.gh;
    bool listed = count && src_rects;
    uint32_t n = listed ? count : 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t l = 0, t = 0, r = std::min(src_t.w, dst_t.w), b = std::min(src_t.h, dst_t.h);
        uint32_t dx = 0, dy = 0;
        if (listed) {
            GuestRect g = ReadGuestRect(src_rects, i);
            l = ScaleUnsigned(g.left, dst_scale);
            t = ScaleUnsigned(g.top, dst_scale_y);
            r = std::min(ScaleUnsigned(g.right, dst_scale), src_t.w);
            b = std::min(ScaleUnsigned(g.bottom, dst_scale_y), src_t.h);
            dx = l;
            dy = t;
            if (dst_points) {
                GuestPoint p = ReadGuestPoint(dst_points, i);
                dx = ScaleUnsigned(p.x, dst_scale);
                dy = ScaleUnsigned(p.y, dst_scale_y);
            }
        }
        if (r <= l || b <= t || dx >= dst_t.w || dy >= dst_t.h) continue;
        r = std::min(r, l + dst_t.w - dx);
        b = std::min(b, t + dst_t.h - dy);
        [BlitEncoder() copyFromTexture:src_t.res
                           sourceSlice:0
                           sourceLevel:0
                          sourceOrigin:MTLOriginMake(l, t, 0)
                            sourceSize:MTLSizeMake(r - l, b - t, 1)
                             toTexture:dst_t.res
                      destinationSlice:0
                      destinationLevel:0
                     destinationOrigin:MTLOriginMake(dx, dy, 0)];
    }
    dst_t.drawn = true;
}

void MetalBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) {
    Target* src_t = FindTarget(src, false);
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "metal: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Settle(*src_t);
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    bool same_scale = src_t->w * dst_t->gw == dst_t->w * src_t->gw && src_t->h * dst_t->gh == dst_t->h * src_t->gh;
    if (!same_scale) CopyByDraw(*src_t, *dst_t, count, src_rects, dst_points);
    else CopyByRegion(*src_t, *dst_t, count, src_rects, dst_points);
}

// ---- textures

id<MTLTexture> MetalBackend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t,
                                               DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (&t == draw_ds_) return nil;
    Settle(t);
    stages.reads.rt[stage] = true;
    if (!t.depth && LatchCpuWrite(t)) {
        if (++reloads_[t.data] <= 3)
            LOGR("metal: sampled target %08X %ux%u reloaded from guest memory (frame %llu)", t.data, t.gw, t.gh,
                 (unsigned long long)presents_);
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    Target* rt = FindTarget(s.rt, false);
    float k = float(t.w) / t.gw;
    if (!t.depth && k > kUpscaledMin && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.reads.wide[stage] = k;
        info.flags |= kTexWide;
    }
    if (NoAlphaFormat(d.format)) info.flags |= kTexOpaque;
    // the console reads the pixels it is about to overwrite: the draw gets a copy
    if (&t == draw_rt_) {
        if (!t.fb) t.fb = NewTexture(kBgra, t.w, t.h, MTLTextureUsageShaderRead);
        if (!t.fb) return nil;
        [BlitEncoder() copyFromTexture:t.res toTexture:t.fb];
        return t.fb;
    }
    if (t.depth) DepthRead(d.format, info);
    return t.res;
}

bool MetalBackend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi) {
    tx = Tex{};
    tx.d = d;
    tx.fmt = MtlTextureFormat(fi.host);
    uint32_t w = d.width, h = d.height;
    tx.levels = d.levels;
    if (fi.compressed && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    tx.levels = std::max(1u, std::min<uint32_t>(tx.levels, uint32_t(std::floor(std::log2(std::max({w, h, d.depth})))) + 1));
    MTLTextureDescriptor* td = [MTLTextureDescriptor new];
    td.pixelFormat = tx.fmt;
    td.width = w;
    td.height = h;
    td.mipmapLevelCount = tx.levels;
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    if (d.depth > 1) {
        td.textureType = MTLTextureType3D;
        td.depth = d.depth;
    } else if (d.cube) {
        td.textureType = MTLTextureTypeCube;
    } else {
        td.textureType = MTLTextureType2D;
    }
    tx.res = [dev_ newTextureWithDescriptor:td];
    return tx.res != nil;
}

void MetalBackend::UploadTexture(Tex& tx, const TextureDesc& d, const uint8_t* p, const uint32_t* colours) {
    uint32_t faces = d.cube ? 6 : 1;
    uint32_t block = BlockBytes(tx.fmt);
    bool volume = d.depth > 1;
    uint32_t w0 = d.width, h0 = d.height;
    if (block && tx.levels == 1) w0 = (w0 + 3) & ~3u, h0 = (h0 + 3) & ~3u;
    std::vector<uint8_t> px, rows;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch = 0;
            DecodeLevel(d, l, f, p, colours, px, pitch);
            uint32_t lw = std::max(1u, w0 >> l), lh = std::max(1u, h0 >> l);
            uint32_t slices = volume ? std::max(1u, d.depth >> l) : 1;
            uint32_t row_bytes = block ? std::max(1u, (lw + 3) / 4) * block : lw * 4;
            uint32_t n_rows = block ? std::max(1u, (lh + 3) / 4) : lh;
            size_t src_rows = pitch ? px.size() / pitch / slices : 0;
            rows.assign(size_t(row_bytes) * n_rows * slices, 0);
            for (uint32_t z = 0; z < slices; z++)
                for (uint32_t y = 0; y < n_rows && y < src_rows; y++)
                    std::memcpy(&rows[(size_t(z) * n_rows + y) * row_bytes], &px[(size_t(z) * src_rows + y) * pitch],
                                std::min(pitch, row_bytes));
            UploadToTexture(tx.res, f, l, lw, lh, slices, rows.data(), row_bytes, n_rows);
        }
    }
}

id<MTLTexture> MetalBackend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                              DrawTextures& stages) {
    if (!d.cube && d.depth == 1 && !fi.depth) {
        auto r = FindReplacement(d.data);
        if (r && ((r->sharpened && !(r->world ? app::Get().world_textures : app::Get().sharp_2d)) || !ReplacementFits(*r, d))) r = nullptr;
        if (id<MTLTexture> t = r ? BindReplacement(*r, d, stages.levels[stage]) : nil) {
            stages.replaced[stage] = true;
            return t;
        }
    }
    if (draw_2d_) Sharpen2D(d);
    else SharpenWorld(d, s.tss[size_t(stage)][kTssAddressU] == 1, s.tss[size_t(stage)][kTssAddressV] == 1);
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    Tex& tx = textures_[key];
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi)) {
        textures_.erase(key);
        return nil;
    }
    if (tx.frame != presents_ + 1 || !same || tx.palette != palette.hash) {
        tx.frame = presents_ + 1;
        tx.palette = palette.hash;
        uint64_t sig = SurfaceSig(d.data, TotalSize(d)) ^ DirtyEpoch(d.data) ^ palette.hash;
        if (sig != tx.sig || !same) {
            tx.sig = sig;
            UploadTexture(tx, d, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), palette.va ? palette.colours : nullptr);
        }
    }
    if (d.cube && !fi.depth) CopyCubeTargets(tx, d);
    stages.levels[stage] = tx.levels;
    if (fi.depth) stages.info[stage].shadow = ShadowDepthKind(d.format);
    return tx.res;
}

id<MTLTexture> MetalBackend::BindReplacement(const Replacement& r, const TextureDesc& d, uint32_t& levels) {
    constexpr uint32_t kFmtA8R8G8B8 = 0x06;
    const uint64_t key = uint64_t(0xFFFFFFFFu) << 32 | d.data;
    if (auto had = textures_.find(key); r.sharpened && (had == textures_.end() || !had->second.res || had->second.sig != r.version) &&
                                        !TakePictureBudget(presents_, r))
        return nil;
    Tex& tx = textures_[key];
    if (tx.frame != presents_ + 1 && !ReplacementHolds(r)) {
        textures_.erase(key);
        return nil;
    }
    tx.frame = presents_ + 1;
    if (!tx.res || tx.sig != r.version) {
        std::vector<std::vector<uint32_t>> pictures = ReplacementLevels(r);
        if (!tx.res || tx.d.width != r.width || tx.d.height != r.height || tx.levels != pictures.size()) {
            TextureDesc rd;
            rd.data = d.data;
            rd.format = kFmtA8R8G8B8;
            rd.width = r.width;
            rd.height = r.height;
            rd.levels = uint32_t(pictures.size());
            if (!CreateTexture(tx, rd, Format(kFmtA8R8G8B8))) {
                textures_.erase(key);
                return nil;
            }
            tx.frame = presents_ + 1;
        }
        uint32_t w = r.width, h = r.height;
        for (uint32_t l = 0; l < pictures.size() && l < tx.levels; l++) {
            UploadToTexture(tx.res, 0, l, w, h, 1, reinterpret_cast<const uint8_t*>(pictures[l].data()), w * 4, h);
            w = std::max(w / 2, 1u);
            h = std::max(h / 2, 1u);
        }
        tx.sig = r.version;
    }
    levels = tx.levels;
    return tx.res;
}

void MetalBackend::CopyCubeTargets(Tex& tx, const TextureDesc& d) {
    if (tx.fmt != kBgra || tx.faces == presents_ + 1) return;
    tx.faces = presents_ + 1;
    for (uint32_t f = 0; f < 6; f++) {
        uint32_t face = d.data + f * FaceSize(d);
        auto it = targets_.find(TargetMapKey(face, false));
        if (it == targets_.end() || TargetStale(face)) continue;
        Target& t = it->second;
        if (t.depth || t.gw != d.width || t.gh != d.height) continue;
        Settle(t);
        id<MTLTexture> src = t.res;
        if (t.w != t.gw || t.h != t.gh) {
            __strong id<MTLTexture>& k = shrinks_[uint64_t(t.gw) << 32 | t.gh];
            if (!k) k = NewTexture(kBgra, t.gw, t.gh, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
            if (!k) continue;
            float uv[4] = {0, 0, 1, 1};
            Blit(k, t.res, uv, {.alpha = true});
            src = k;
        }
        [BlitEncoder() copyFromTexture:src
                           sourceSlice:0
                           sourceLevel:0
                          sourceOrigin:MTLOriginMake(0, 0, 0)
                            sourceSize:MTLSizeMake(t.gw, t.gh, 1)
                             toTexture:tx.res
                      destinationSlice:f
                      destinationLevel:0
                     destinationOrigin:MTLOriginMake(0, 0, 0)];
    }
}

id<MTLTexture> MetalBackend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    stages.info[stage] = {};
    stages.reads.rt[stage] = false;
    stages.reads.k[stage] = 1;
    stages.reads.wide[stage] = 0;
    stages.levels[stage] = 1;
    stages.replaced[stage] = false;
    uint32_t header = s.textures[size_t(stage)];
    TextureDesc d;
    if (!header || !Describe(header, d)) return nil;
    const FormatInfo& fi = Format(d.format);
    stages.info[stage].kind = TextureKind(d);
    stages.info[stage].rect = d.linear;
    auto tit = targets_.find(SizedTargetKey(d.data, fi.depth, d.width, d.height));
    if (tit == targets_.end() && !fi.depth && DepthBytesOrder(d.format))
        tit = targets_.find(SizedTargetKey(d.data, true, d.width, d.height));
    if (tit == targets_.end() && fi.depth && d.linear) {
        auto big = targets_.find(TargetMapKey(d.data, true));
        if (big != targets_.end() && big->second.gw >= d.width && big->second.gh >= d.height) {
            tit = big;
            stages.reads.size[stage][0] = big->second.gw;
            stages.reads.size[stage][1] = big->second.gh;
        }
    }
    if (tit != targets_.end() && !fi.depth && !tit->second.depth && d.linear &&
        (tit->second.gw > d.width || tit->second.gh > d.height) && tit->second.gw >= d.width &&
        tit->second.gh >= d.height) {
        stages.reads.size[stage][0] = tit->second.gw;
        stages.reads.size[stage][1] = tit->second.gh;
    }
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed)
        return BindTargetTexture(s, stage, d, tit->second, stages);
    return BindGuestTexture(s, stage, d, fi, stages);
}

// ---- the draw

id<MTLSamplerState> MetalBackend::Sampler(const D3D12_SAMPLER_DESC& d) {
    uint64_t h = Hash(&d, sizeof(d));
    __strong id<MTLSamplerState>& st = samplers_[h];
    if (st) return st;
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    int f = int(d.Filter);
    bool aniso = (f & 0x55) == 0x55 && (f & 0x40);
    auto filt = [](int v) { return v ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest; };
    sd.minFilter = aniso ? MTLSamplerMinMagFilterLinear : filt((f >> 4) & 1);
    sd.magFilter = aniso ? MTLSamplerMinMagFilterLinear : filt((f >> 2) & 1);
    sd.mipFilter = aniso || (f & 1) ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    sd.maxAnisotropy = aniso ? std::clamp<NSUInteger>(d.MaxAnisotropy, 1, 16) : 1;
    sd.sAddressMode = MtlAddress(d.AddressU);
    sd.tAddressMode = MtlAddress(d.AddressV);
    sd.rAddressMode = MtlAddress(d.AddressW);
    // Metal has three border colours: the nearest
    const float* b = d.BorderColor;
    if (d.AddressU == D3D12_TEXTURE_ADDRESS_MODE_BORDER || d.AddressV == D3D12_TEXTURE_ADDRESS_MODE_BORDER) {
        bool exact = (b[3] == 0 && b[0] == 0 && b[1] == 0 && b[2] == 0) ||
                     (b[3] == 1 && ((b[0] == 0 && b[1] == 0 && b[2] == 0) || (b[0] == 1 && b[1] == 1 && b[2] == 1)));
        if (!exact) LOG_FIRST(8, "metal: border colour %.2f %.2f %.2f %.2f shown as the nearest of black, transparent, white",
                              b[0], b[1], b[2], b[3]);
    }
    sd.borderColor = b[3] < 0.5f ? MTLSamplerBorderColorTransparentBlack
                     : (b[0] + b[1] + b[2]) > 1.5f ? MTLSamplerBorderColorOpaqueWhite
                                                    : MTLSamplerBorderColorOpaqueBlack;
    sd.lodMinClamp = d.MinLOD;
    sd.lodMaxClamp = std::min(d.MaxLOD, 1000.0f);
    if (f & 0x80) sd.compareFunction = MtlCmp(d.ComparisonFunc);
    st = [dev_ newSamplerStateWithDescriptor:sd];
    return st;
}

MetalBackend::Pipeline* MetalBackend::EnsurePipeline(const PsoKey& k, const Assembled& a, const Compiled* vs,
                                                     const Compiled* ps, uint64_t vh, uint64_t ph) {
    // the strides are part of a Metal pipeline (its vertex descriptor)
    uint32_t strides[16] = {};
    for (uint32_t i = 0; i < a.buffer_count; i++) strides[i] = a.buffers[i].stride;
    uint64_t h = Hash(strides, sizeof(strides), Hash(&k, sizeof(k), vh ^ (ph * 31)));
    auto it = pipelines_.find(h);
    if (it != pipelines_.end()) return &it->second;
    Pipeline& p = pipelines_[h];
    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs->fn;
    pd.fragmentFunction = ps->fn;
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    bool used[16] = {};
    for (uint32_t loc = 0; loc < 16; loc++) {
        int r = vs->attr_reg[loc];
        if (r < 0 || !((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        MTLVertexFormat fmt = MtlVertexFormat(e & 0xFF);
        uint32_t buffer = (e >> 8) & 0xFF;
        if (fmt == MTLVertexFormatInvalid || buffer >= 16) {
            LOG_FIRST(8, "metal: vertex element format %u of v%d not handled", e & 0xFF, r);
            continue;
        }
        vd.attributes[loc].format = fmt;
        vd.attributes[loc].bufferIndex = kVertexBufferBase + buffer;
        vd.attributes[loc].offset = e >> 16;
        used[buffer] = true;
    }
    for (uint32_t b = 0; b < 16; b++) {
        if (!used[b]) continue;
        uint32_t stride = strides[b];
        if (stride == 0) {
            vd.layouts[kVertexBufferBase + b].stepFunction = MTLVertexStepFunctionConstant;
            vd.layouts[kVertexBufferBase + b].stepRate = 0;
            vd.layouts[kVertexBufferBase + b].stride = 16;
        } else {
            vd.layouts[kVertexBufferBase + b].stride = (stride + 3) & ~3u;
        }
    }
    pd.vertexDescriptor = vd;
    if (k.has_rt) {
        auto* ca = pd.colorAttachments[0];
        ca.pixelFormat = kBgra;
        ca.blendingEnabled = k.blend != 0;
        ca.sourceRGBBlendFactor = MtlBlend(Blend(k.src, false));
        ca.destinationRGBBlendFactor = MtlBlend(Blend(k.dst, false));
        ca.rgbBlendOperation = MtlBlendOp(BlendOp(k.op));
        ca.sourceAlphaBlendFactor = MtlBlend(Blend(k.src, true));
        ca.destinationAlphaBlendFactor = MtlBlend(Blend(k.dst, true));
        ca.alphaBlendOperation = MtlBlendOp(BlendOp(k.op));
        ca.writeMask = ((k.wmask & 1) ? MTLColorWriteMaskRed : 0) | ((k.wmask & 2) ? MTLColorWriteMaskGreen : 0) |
                       ((k.wmask & 4) ? MTLColorWriteMaskBlue : 0) | ((k.wmask & 8) ? MTLColorWriteMaskAlpha : 0);
    }
    if (k.has_ds) pd.depthAttachmentPixelFormat = pd.stencilAttachmentPixelFormat = kDepthFormat;
    if (k.topo == uint32_t(Topology::Points)) pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint;
    pd.rasterSampleCount = std::max<uint32_t>(k.samples, 1);
    NSError* err = nil;
    p.state = [dev_ newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!p.state) {
        LOG_FIRST(16, "metal: pipeline failed: %s", err ? [[err localizedDescription] UTF8String] : "?");
        return &p;
    }
    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = k.zen ? MtlCmp(Cmp(k.zfunc)) : MTLCompareFunctionAlways;
    dd.depthWriteEnabled = k.zen && k.zwrite;
    if (k.sten) {
        MTLStencilDescriptor* st = [MTLStencilDescriptor new];
        st.stencilCompareFunction = MtlCmp(Cmp(k.sfunc));
        st.stencilFailureOperation = MtlStencilOp(StencilOp(k.sfail));
        st.depthFailureOperation = MtlStencilOp(StencilOp(k.szfail));
        st.depthStencilPassOperation = MtlStencilOp(StencilOp(k.spass));
        st.readMask = k.smask & 0xFF;
        st.writeMask = k.swmask & 0xFF;
        dd.frontFaceStencil = dd.backFaceStencil = st;
    }
    p.depth = [dev_ newDepthStencilStateWithDescriptor:dd];
    bool points = k.topo == uint32_t(Topology::Points);
    p.fill = k.fill == kFillWireframe && !points ? MTLTriangleFillModeLines : MTLTriangleFillModeFill;
    p.cull = k.cull && !points ? MTLCullModeBack : MTLCullModeNone;
    p.winding = k.ccw && !points ? MTLWindingCounterClockwise : MTLWindingClockwise;
    // D3D's bias is in steps of the 24 bit depth format; with 32 bit float depth near 1 one step is 2^-24 too
    p.bias = float(k.zbias);
    p.slope = k.zslope;
    p.ok = true;
    return &p;
}

void MetalBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (const State* sprites = PointSpriteStages(s, prim)) return Draw(*sprites, prim, start, count, indices);
    if (s.blank_format && s.textures[0] && (::kernel::R32(s.textures[0] + 12) >> 8 & 0xFF) == s.blank_format) {
        Clear(s, 0, nullptr, kClearTarget, 0xFF000000u, 0, 0);
        return;
    }
    if (DepthCopy(s)) return;
    if (diag::SkippedDraw(s.vs_handle)) return;
    DrawAdjust adjust;
    if (g_draw_filter) g_draw_filter(s, adjust);
    if (adjust.skip) return;
    Assembled& a = asm_;
    if (!Assemble(s, prim, start, count, indices, a)) return;
    if (!BindTargets(s)) return;

    // vertex shader
    DrawShaders sh = ClassifyShaders(s, a, count);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    const bool points = a.topology == Topology::Points;
    // a point is its console pixel: the target's host pixels per console pixel
    float point_size = 0;
    if (points) point_size = std::max(1.0f, std::round(scale_));
    const Compiled* vs = nullptr;
    {
        uint64_t vkey = sh.vkey ^ (points ? 0x9F1Dull : 0);
        auto vit = shader_src_.find(vkey);
        if (vit != shader_src_.end()) {
            sh.vh = vit->second;
            auto c = shaders_.find(sh.vh);
            vs = c != shaders_.end() && c->second.fn ? &c->second : nullptr;
        } else {
            std::string src = VertexHlsl(s, a, sh);
            vs = Compile(sh.quad ? QuadHlsl(std::move(src)) : src, "main", true, sh.vh, point_size);
            shader_src_[vkey] = sh.vh;
        }
    }
    // stage textures, which may draw copies, then the targets again
    DrawTextures stages;
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    draw_2d_ = a.pretransformed || ScreenDraw(s);
    for (int i = 0; i < 4; i++) stages.tex[i] = BindTexture(s, i, stages);
    if (!BindTargets(s)) return;
    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad;
    // alpha tested, not blended, onto MSAA twins: Settings::alpha_coverage (not quads, whose shader is rewritten)
    if (app::Get().alpha_coverage && bound_ms_ && !sh.quad && SimpleState(s, kMAlphaTestEnable) && !SimpleState(s, kMBlendEnable))
        pk.coverage = msaa_;
    static const bool wbuffer = [] {
        const char* e = std::getenv("XBE_WBUFFER");
        return !(e && *e == '0');
    }();
    pk.wbuf = wbuffer && s.rs[kRsZEnable] == 2 && bound_ds_ ? 1 : 0;
    sh.pkey = Hash(&pk, sizeof(pk));
    const Compiled* ps = nullptr;
    {
        auto pit = shader_src_.find(sh.pkey ^ kPixelKeySalt);
        if (pit != shader_src_.end()) {
            sh.ph = pit->second;
            auto c = shaders_.find(sh.ph);
            ps = c != shaders_.end() && c->second.fn ? &c->second : nullptr;
        } else {
            ps = Compile(PixelHlsl(pk), "main", false, sh.ph);
            shader_src_[sh.pkey ^ kPixelKeySalt] = sh.ph;
        }
    }
    if (!vs || !ps) return;

    PsoKey k;
    PipelineTargets pt{.rt = bound_rt_ != nullptr, .rtfmt = bound_rt_ ? uint32_t(DXGI_FORMAT_B8G8R8A8_UNORM) : 0u,
                       .ds = bound_ds_ != nullptr, .ds_scale = bound_ds_ ? float(bound_ds_->h) / bound_ds_->gh : 1.0f,
                       .samples = bound_ms_ ? msaa_ : 1};
    BuildPipelineKey(s, a, sh.vh, sh.ph, pt, k);
    if (!adjust.depth_test) k.zen = k.zwrite = 0;
    k.zbias += adjust.depth_bias;
    float wdepth[4] = {float(k.zbias) * s.zscale / kDepth24Max, k.zslope, 1.0f / (s.zscale + 1.0f),
                       sh.prog ? 1.0f : s.zscale * s.inv_wfar};
    if (pk.wbuf) k.zbias = 0, k.zslope = 0;
    Pipeline* p = EnsurePipeline(k, a, vs, ps, sh.vh, sh.ph);
    if (!p->ok) return;

    // constants
    GuestExtent size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    size = DrawnExtent(s, size);
    DrawConstants c{};
    std::memcpy(c.wdepth, wdepth, sizeof(wdepth));
    float stretch = bound_rt_ ? XStretch(aspect_, guest_h_, bound_rt_->gw, bound_rt_->gh) : 0;
    c.hud[0] = HudSqueeze(s, a, aspect_, stretch);
    c.hud[2] = DepthTested(s) ? 1.0f : 0.0f;
    c.hud[3] = pk.wbuf ? 1.0f : 0.0f;
    if (bound_rt_)
        NoteSqueeze(s, a, bound_rt_->data, bound_rt_->gw, bound_rt_->gh, bound_rt_->w, aspect_, stretch, c.hud[0]);
    c.hud[1] = (bound_rt_ ? bound_rt_->gw : guest_w_) * 0.5f / s.aa_x;
    if (c.hud[0] != 1 && !s.squeeze_beyond) {
        Span span = DrawSpan(s, a, c.hud[1] * 2);
        if (span == kSpanNone) c.hud[0] = -c.hud[0];
        if (span == kSpanViewport) c.hud[0] = 1;
    }
    SqueezeBeyond(s, c.hud);
    Pillarbox(s, a, aspect_, stretch, c.hud);
    FillSurfaceConstants(s, size, c);
    if (k.topo == uint32_t(Topology::Points) || k.topo == uint32_t(Topology::Lines)) {
        const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
        c.surf[3] = t && t->gh ? float(t->h) * s.aa_y / float(t->gh) : 1.0f;
    }
    FillFogConstants(s, sh.prog, c);
    FillCombinerConstants(s, c);
    FillTextureConstants(s, stages.reads, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    D3D12_SAMPLER_DESC sds[4];
    {
        bool clamp = UnitTexcoords(a);
        uint32_t aniso = app::Get().aniso;
        for (int i = 0; i < 4; i++) {
            sds[i] = StageSampler(s, i, stages.levels[i], stages.info[i].shadow, clamp, aniso, stages.replaced[i]);
            c.lodbias[i] = sds[i].MipLODBias;
        }
    }

    // the viewport covers the target, the guest viewport is in the shader; the scissor clips to it
    const Target* b = bound_rt_ ? bound_rt_ : bound_ds_;
    float W = float(b->w), H = float(b->h);
    float sx = W / size.w, sy = b->gh ? H * s.aa_y / b->gh : sx;
    LONG x0 = LONG(SqueezedX(c.hud, s.viewport.x) * sx);
    LONG y0 = LONG(s.viewport.y * sy);
    LONG x1 = LONG(SqueezedX(c.hud, s.viewport.x + s.viewport.w) * sx);
    LONG y1 = LONG((s.viewport.y + s.viewport.h) * sy);
    if (s.scissor_count) {
        x0 = LONG(SqueezedX(c.hud, float(s.scissors[0])) * sx);
        y0 = LONG(s.scissors[1] * sy);
        x1 = LONG(SqueezedX(c.hud, float(s.scissors[2])) * sx);
        y1 = LONG(s.scissors[3] * sy);
    }
    if (s.squeeze_margin) {
        const LONG edge = LONG(SqueezedX(c.hud, s.squeeze_margin < 0 ? 0.0f : c.hud[1] * 2) * sx);
        x0 = s.squeeze_margin < 0 ? 0 : edge;
        x1 = s.squeeze_margin < 0 ? edge : LONG(W);
    }
    x0 = std::clamp<LONG>(x0, 0, LONG(W));
    y0 = std::clamp<LONG>(y0, 0, LONG(H));
    x1 = std::clamp<LONG>(x1, 0, LONG(W));
    y1 = std::clamp<LONG>(y1, 0, LONG(H));
    if (x1 <= x0 || y1 <= y0) return;  // nothing inside the scissor

    // the render pass and its state
    id<MTLRenderCommandEncoder> enc = Render(bound_rt_ ? (bound_ms_ ? bound_rt_->ms : bound_rt_->res) : nil,
                                             bound_ds_ ? (bound_ms_ ? bound_ds_->ms : bound_ds_->res) : nil);
    if (!enc) return;
    [enc setRenderPipelineState:p->state];
    [enc setDepthStencilState:p->depth];
    [enc setStencilReferenceValue:SimpleState(s, kMStencilRef) & 0xFF];
    [enc setCullMode:p->cull];
    [enc setFrontFacingWinding:p->winding];
    [enc setTriangleFillMode:p->fill];
    [enc setDepthBias:p->bias slopeScale:p->slope clamp:0];
    float bf[4];
    BlendFactor(s, bf);
    [enc setBlendColorRed:bf[0] green:bf[1] blue:bf[2] alpha:bf[3]];
    [enc setViewport:MTLViewport{0, 0, W, H, 0, 1}];
    [enc setScissorRect:MTLScissorRect{NSUInteger(x0), NSUInteger(y0), NSUInteger(x1 - x0), NSUInteger(y1 - y0)}];
    [enc setVertexBytes:s.vsc.data() length:sizeof(s.vsc) atIndex:0];
    [enc setFragmentBytes:s.vsc.data() length:sizeof(s.vsc) atIndex:0];
    [enc setVertexBytes:&c length:sizeof(c) atIndex:1];
    [enc setFragmentBytes:&c length:sizeof(c) atIndex:1];
    for (int i = 0; i < 4; i++) {
        [enc setFragmentTexture:stages.tex[i] atIndex:i];
        [enc setFragmentSamplerState:Sampler(sds[i]) atIndex:i];
    }

    // geometry: every stream in upload memory
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& vb = a.buffers[i];
        if (!vb.bytes) continue;
        Upload u = Allocate(vb.bytes, 16);
        std::memcpy(u.p, vb.src, vb.bytes);
        [enc setVertexBuffer:u.buf offset:u.offset atIndex:kVertexBufferBase + i];
    }
    MTLPrimitiveType type;
    switch (PrimitiveTopology(a)) {
        case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: type = MTLPrimitiveTypePoint; break;
        case D3D_PRIMITIVE_TOPOLOGY_LINELIST: type = MTLPrimitiveTypeLine; break;
        case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: type = MTLPrimitiveTypeLineStrip; break;
        case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: type = MTLPrimitiveTypeTriangleStrip; break;
        default: type = MTLPrimitiveTypeTriangle; break;
    }
    bool counting = s.simple[kMCountEnable >> 2] != 0;
    if (counting) {
        if (vis_used_ >= kVisibilitySlots) counting = false;
        else {
            std::memset(static_cast<uint8_t*>([vis_ contents]) + vis_used_ * 8, 0, 8);
            [enc setVisibilityResultMode:MTLVisibilityResultModeCounting offset:vis_used_ * 8];
        }
    }
    Upload ib{};
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        ib = Allocate(ibytes, 4);
        std::memcpy(ib.p, a.index_src, ibytes);
    }
    auto draw = [&] {
        if (a.index_count)
            [enc drawIndexedPrimitives:type
                            indexCount:a.index_count
                             indexType:a.index16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                           indexBuffer:ib.buf
                     indexBufferOffset:ib.offset
                         instanceCount:1
                            baseVertex:a.base_vertex
                          baseInstance:0];
        else
            [enc drawPrimitives:type vertexStart:0 vertexCount:a.draw_count];
    };
    // a line one console pixel wide: n x n copies, each moved by a whole host pixel. Blended lines are drawn once (the
    // copies would blend over each other), as are lines under a visibility test (they would count n x n times)
    const int wide = (type == MTLPrimitiveTypeLine || type == MTLPrimitiveTypeLineStrip) && !k.blend && !counting
                         ? std::clamp(int(std::lround(c.surf[3])), 1, 8)
                         : 1;
    if (wide > 1) {
        for (int dy = 0; dy < wide; dy++)
            for (int dx = 0; dx < wide; dx++) {
                double ox = dx - (wide - 1) / 2, oy = dy - (wide - 1) / 2;
                [enc setViewport:MTLViewport{ox, oy, W, H, 0, 1}];
                draw();
            }
        [enc setViewport:MTLViewport{0, 0, W, H, 0, 1}];
    } else {
        draw();
    }
    perf_.draws++;
    if (counting) {
        [enc setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
        counted_.push_back({vis_used_, SamplesPerPixel(), serial_});
        vis_used_++;
    }
}

// ---- visibility tests

float MetalBackend::SamplesPerPixel() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw || !t->gh) return 1;
    return float(t->w) * float(t->h) / (float(t->gw) * float(t->gh)) * float(bound_ms_ ? msaa_ : 1);
}

void MetalBackend::DeliverReports(bool wait) {
    constexpr uint32_t kTime = 0, kTimeHigh = 4, kCount = 8, kStatus = 12;
    while (!reports_.empty()) {
        Report& r = reports_.front();
        uint64_t need = 0;
        for (const Slot& sl : r.slots) need = std::max(need, sl.serial);
        if (need >= serial_) {  // still being recorded
            if (!wait) return;
            perf_.report_waits++;
            Commit(true);
        }
        if (need > completed_.load()) {
            if (!wait) return;
            while (need > completed_.load()) std::this_thread::yield();
        }
        double pixels = 0;
        for (const Slot& sl : r.slots) {
            auto it = vis_by_serial_.find(sl.serial);
            if (it == vis_by_serial_.end()) continue;
            uint64_t n = static_cast<const uint64_t*>([it->second contents])[sl.index];
            pixels += double(n) / sl.samples_per_pixel;
        }
        kernel::W32(r.va + kTime, uint32_t(presents_));
        kernel::W32(r.va + kTimeHigh, 0);
        kernel::W32(r.va + kCount, uint32_t(std::lround(pixels)));
        std::atomic_thread_fence(std::memory_order_release);
        kernel::W32(r.va + kStatus, 0);
        reports_.pop_front();
    }
    // results nobody waits for any more
    uint64_t oldest = reports_.empty() ? serial_ : reports_.front().serial;
    for (const Slot& sl : counted_) oldest = std::min(oldest, sl.serial);
    for (auto it = vis_by_serial_.begin(); it != vis_by_serial_.end();) {
        if (it->first < oldest && it->first <= completed_.load()) it = vis_by_serial_.erase(it);
        else ++it;
    }
}

// ---- present

void MetalBackend::UploadGammaRamp() {
    constexpr int kRed = 0, kGreen = 256, kBlue = 512;
    ramp_dirty_ = false;
    uint8_t px[256 * 4];
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        px[i * 4] = ramp_[kBlue + i];
        px[i * 4 + 1] = ramp_[kGreen + i];
        px[i * 4 + 2] = ramp_[kRed + i];
        px[i * 4 + 3] = 255;
        identity &= ramp_[kRed + i] == i && ramp_[kGreen + i] == i && ramp_[kBlue + i] == i;
    }
    UploadToTexture(lut_, 0, 0, 256, 1, 1, px, sizeof(px), 1);
    gamma_ = !identity;
}

id<MTLTexture> MetalBackend::SmaaFrame(Target& t) {
    if (!smaa_area_) {
        smaa_area_ = NewTexture(MTLPixelFormatRG8Unorm, kSmaaAreaWidth, kSmaaAreaHeight, MTLTextureUsageShaderRead);
        smaa_search_ = NewTexture(MTLPixelFormatR8Unorm, kSmaaSearchWidth, kSmaaSearchHeight, MTLTextureUsageShaderRead);
        if (!smaa_area_ || !smaa_search_) {
            smaa_area_ = nil;
            return nil;
        }
        UploadToTexture(smaa_area_, 0, 0, kSmaaAreaWidth, kSmaaAreaHeight, 1, kSmaaArea, kSmaaAreaWidth * 2, kSmaaAreaHeight);
        UploadToTexture(smaa_search_, 0, 0, kSmaaSearchWidth, kSmaaSearchHeight, 1, kSmaaSearch, kSmaaSearchWidth,
                        kSmaaSearchHeight);
    }
    if (!smaa_[0] || smaa_[0].width != t.w || smaa_[0].height != t.h) {
        for (auto& p : smaa_) {
            p = NewTexture(kBgra, t.w, t.h, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
            if (!p) {
                smaa_[0] = nil;
                return nil;
            }
        }
    }
    const float c[8] = {0, 0, 1, 1, 1.0f / t.w, 1.0f / t.h, float(t.w), float(t.h)};
    auto pass = [&](const char* vse, const char* pse, id<MTLTexture> dst, std::initializer_list<id<MTLTexture>> tex) {
        uint64_t h;
        const Compiled* vs = Compile(kSmaaHlsl, vse, true, h);
        const Compiled* ps = Compile(kSmaaHlsl, pse, false, h);
        if (!vs || !ps) return false;
        std::vector<id<MTLTexture>> list(tex);
        // the edges pass leaves pixels without an edge as they were: each pass starts from black
        Cmd();
        EndEncoders();
        MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd.colorAttachments[0].texture = dst;
        rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
        rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
        rpd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        [[cmd_ renderCommandEncoderWithDescriptor:rpd] endEncoding];
        FullScreen(dst, nil, vs, ps, c, 8, list.data(), uint32_t(list.size()), false, 0);
        return true;
    };
    if (!pass("vsedge", "psedge", smaa_[0], {t.res}) ||
        !pass("vsweight", "psweight", smaa_[1], {smaa_[0], smaa_area_, smaa_search_}) ||
        !pass("vsblend", "psblend", smaa_[2], {t.res, smaa_[1]}))
        return nil;
    return smaa_[2];
}

void MetalBackend::RenderUi(id<MTLRenderCommandEncoder> enc, MTLRenderPassDescriptor* rpd) {
    if (!ui_) return;
    std::lock_guard lock(app::UiLock());
    app::FeedPad();
    ImGui_ImplMetal_NewFrame(rpd);
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    app::DrawHud(fps_);
    app::DrawOverlay();
    if (app::kGame.ui) app::kGame.ui(fps_);
    ImGui::Render();
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cmd_, enc);
}

void MetalBackend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;
    constexpr size_t kTexturesKept = 2048;
    constexpr uint64_t kIdleFrames = 600;
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) it = textures_.erase(it);
        else ++it;
    }
}

void MetalBackend::CountFps() {
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_ = fps_n_ / el;
        fps_n_ = 0;
        fps_t_ = now;
    }
}

void MetalBackend::Present(const Surface& front) {
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (t) Settle(*t);
    if (ramp_dirty_) UploadGammaRamp();
    // F12, or every XBE_SHOT_EVERY frames: the frame as shown, before the interface
    static const uint64_t shot_every = [] {
        const char* e = std::getenv("XBE_SHOT_EVERY");
        return e ? uint64_t(std::atoll(e)) : 0ull;
    }();
    const bool shot = app::TakeScreenshotRequest() || (shot_every && presents_ && presents_ % shot_every == 0);
    id<MTLTexture> frame = t ? t->res : nil;
    if (t && app::Get().smaa)
        if (id<MTLTexture> smoothed = SmaaFrame(*t)) frame = smoothed;
    @autoreleasepool {
        id<CAMetalDrawable> drawable = nil;
        if (layer_) {
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(app::Window()), &pw, &ph);
            CGSize want = CGSizeMake(std::max(pw, 1), std::max(ph, 1));
            if (!CGSizeEqualToSize(layer_.drawableSize, want)) layer_.drawableSize = want;
            layer_.displaySyncEnabled = app::Get().vsync;
            auto t0 = std::chrono::steady_clock::now();
            drawable = [layer_ nextDrawable];
            perf_.drawable_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
        if (drawable) {
            Cmd();
            EndEncoders();
            id<MTLTexture> bb = drawable.texture;
            MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
            rpd.colorAttachments[0].texture = bb;
            rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
            rpd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            enc_ = [cmd_ renderCommandEncoderWithDescriptor:rpd];
            enc_colour_ = bb;
            enc_depth_ = nil;
            if (frame) {
                // the frame at its shape (16:9 or 4:3, ultrawide the screen's), centred on black
                float aspect = app::FrameAspect();
                float w = float(bb.width), h = float(bb.height);
                float vw = std::min(w, h * aspect), vh = vw / aspect;
                MTLViewport vp{(w - vw) / 2, (h - vh) / 2, vw, vh, 0, 1};
                float uv[4] = {0, 0, 1, 1};
                Blit(bb, frame, uv, {.gamma = gamma_, .final = true}, &vp);
            }
            if (shot) {
                std::vector<uint8_t> px;
                if (ReadBack(bb, uint32_t(bb.width), uint32_t(bb.height), px))
                    app::SaveScreenshot(std::move(px), uint32_t(bb.width), uint32_t(bb.height));
            }
            id<MTLRenderCommandEncoder> enc = Render(bb, nil);
            [enc setViewport:MTLViewport{0, 0, double(bb.width), double(bb.height), 0, 1}];
            [enc setScissorRect:MTLScissorRect{0, 0, bb.width, bb.height}];
            RenderUi(enc, rpd);
            EndEncoders();
            [cmd_ presentDrawable:drawable];
        }
        Commit();
    }
    DeliverReports(false);
    presents_++;
    ReleaseOldTextures();
    CountFps();
    ApplySettings();
    static const bool perf = std::getenv("XBE_PERF") != nullptr;
    if (perf && std::chrono::steady_clock::now() - perf_t_ > std::chrono::seconds(10)) {
        LOGR("metal perf: %.1f fps; per frame %.0f draws, %.1f commits, %.2f read-backs, %.2f report waits; "
             "%.2f ms waiting on the GPU, %.2f ms for a drawable", fps_, perf_.draws / (fps_ * 10), perf_.commits / (fps_ * 10),
             perf_.readbacks / (fps_ * 10), perf_.report_waits / (fps_ * 10), perf_.wait_ms / (fps_ * 10),
             perf_.drawable_ms / (fps_ * 10));
        perf_ = {};
        perf_t_ = std::chrono::steady_clock::now();
    }
}

void MetalBackend::ApplySettings() {
    // the msaa setting 1, 2, 3: MSAA 2x, 4x, 8x; a change settles and drops every twin, the next draws make new ones
    static const uint32_t kSamples[] = {1, 2, 4, 8};
    uint32_t want_ms = kSamples[std::min<uint32_t>(app::Get().msaa, 3)];
    while (want_ms > 1 && ![dev_ supportsTextureSampleCount:want_ms]) want_ms /= 2;
    if (want_ms != msaa_) {
        for (auto& [key, t] : targets_) DropMs(t);
        msaa_ = want_ms;
        bound_rt_ = bound_ds_ = nullptr;
        bound_ms_ = false;
        LOGR("metal: MSAA %ux", msaa_);
    }
    float aspect = app::FrameAspect();
    if (std::fabs(aspect - aspect_) > 1e-4f) {
        aspect_ = aspect;
        RescaleTargets();
    }
    float want = app::RenderScale(guest_h_);
    static float pending = 0;
    static std::chrono::steady_clock::time_point since;
    auto now = std::chrono::steady_clock::now();
    if (std::fabs(want - scale_) <= 1e-4f) return;
    if (std::fabs(want - pending) > 1e-4f) {
        pending = want;
        since = now;
        return;
    }
    if (now - since < std::chrono::milliseconds(500)) return;
    scale_ = want;
    RescaleTargets();
}

void MetalBackend::RescaleTargets() {
    for (auto& [key, t] : targets_) DropMs(t);
    EndEncoders();
    bound_rt_ = bound_ds_ = nullptr;
    for (auto it = targets_.begin(); it != targets_.end();) {
        Target& old = it->second;
        if (old.depth) {
            it = targets_.erase(it);
            continue;
        }
        Target t;
        t.gw = old.gw;
        t.gh = old.gh;
        if (!CreateTarget(t)) {
            it = targets_.erase(it);
            continue;
        }
        float uv[4] = {0, 0, 1, 1};
        Blit(t.res, old.res, uv, {.alpha = true});
        t.data = old.data;
        t.bytes = old.bytes;
        t.epoch = old.epoch;
        t.sig = old.sig;
        t.drawn = old.drawn;
        t.linear32 = old.linear32;
        t.swizzled32 = old.swizzled32;
        t.read = std::move(old.read);
        old = std::move(t);
        ++it;
    }
    LOGR("metal: render scale %.2f, %zu targets kept", scale_, targets_.size());
}

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<MetalBackend>(); }
std::unique_ptr<Backend> CreateFallbackBackend() { return nullptr; }
std::unique_ptr<Backend> CreateD3D11Backend() { return nullptr; }

}  // namespace gpu
