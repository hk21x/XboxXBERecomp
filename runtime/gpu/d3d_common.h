#pragma once
// What the Direct3D 12 and Direct3D 11 renderers share: console state to host state, pipeline and shader keys, the
// draw constants, the full screen pass shaders and the guest's rect lists. Windows only (D3D12 enums: the D3D11 ones
// have the same values and layouts).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3dcommon.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "gpu/shaders.h"
#include "gpu/state.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"

namespace gpu {

// full screen passes: Blit (b1 uvrect, opts; t0 image, t1 gamma lookup; s0 linear, s1 point), a depth copy and an
// MSAA depth resolve (t0 depth)
extern const char* kBlitHlsl;
extern const char* kDepthHlsl;
extern const char* kDepthResolveHlsl;

float Bits(uint32_t v);
void Color(uint32_t argb, float* o);  // console ARGB to RGBA floats
// the draw's blend factor (D3DRS_BLENDCOLOR): its alpha in all four when the blend uses constant alpha (0x8003,
// 0x8004) and not constant colour, since D3D's blend factor scales each channel by its own component
void BlendFactor(const State& s, float* o);
// console values (GL numbering) to host state
D3D12_BLEND Blend(uint32_t v, bool alpha);
D3D12_BLEND_OP BlendOp(uint32_t v);
D3D12_COMPARISON_FUNC Cmp(uint32_t v);
D3D12_STENCIL_OP StencilOp(uint32_t v);
D3D12_TEXTURE_ADDRESS_MODE Address(uint32_t v);
DXGI_FORMAT HostFormat(Host h);
uint8_t ShadowDepthKind(uint32_t format);  // TexInfo::shadow of a depth texture
uint32_t GuestPitch(uint32_t width);       // bytes per row of a 32 bit surface in guest memory
TexKind TextureKind(const TextureDesc& d);

// SurfaceSig: a surface's bytes hashed, up to kSigWholeMax whole, a bigger one as evenly spaced samples
constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;
uint64_t SurfaceSig(uint32_t data, uint32_t total);

constexpr float kUpscaledMin = 1.01f;  // host texels per console texel past rounding: the target is upscaled
constexpr float kDepth24Max = 16777215.0f;  // 2^24 minus 1: a 24 bit depth buffer's units per 1.0
constexpr uint32_t kFillWireframe = 0x1B01;  // D3DFILL_WIREFRAME
constexpr uint32_t kCullCw = 0x900;          // D3DCULL_CW
constexpr uint32_t kQuadVertices = 6;        // a quad drawn as two triangles
constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;  // pixel shader keys apart from vertex shader keys
constexpr float kTapsSlack = 1e-3f;          // a drawn copy's taps round down a ratio a hair over a whole number

// a pipeline's state, hashed whole as the cache key
struct PsoKey {
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t blend;
    uint32_t src;
    uint32_t dst;
    uint32_t op;
    uint32_t wmask;
    uint32_t zen;
    uint32_t zwrite;
    uint32_t zfunc;
    uint32_t sten;
    uint32_t sfunc;
    uint32_t smask;
    uint32_t swmask;
    uint32_t sfail;
    uint32_t szfail;
    uint32_t spass;
    uint32_t cull;
    uint32_t ccw;
    uint32_t fill;
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;
    uint32_t samples;  // MSAA, 0 or 1 none
};

// the bound targets as the pipeline key sees them
struct PipelineTargets {
    bool rt = false;
    uint32_t rtfmt = 0;     // DXGI format of the colour target
    bool ds = false;
    float ds_scale = 1;     // host pixels per console pixel down the depth target
    uint32_t samples = 1;
};

// a draw's shaders: what they are made from, their cache keys, the hashes Compile returned and the compiled blobs
struct DrawShaders {
    bool prog = false;  // a vertex program, else the fixed function pipeline
    bool quad = false;
    bool fixed_transform = false;  // fixed function on untransformed vertices: the shader reads matrices and lights
    FixedKey fk;
    uint64_t vkey = 0;
    uint64_t vh = 0;
    uint64_t pkey = 0;
    uint64_t ph = 0;
    ID3DBlob* vs_code = nullptr;
    ID3DBlob* ps_code = nullptr;
};

// the render target's console size in pixels, or the depth buffer's without one
struct GuestExtent {
    uint32_t w;
    uint32_t h;
};

// the size in the pixels draws address (State::aa_x, aa_y)
inline GuestExtent DrawnExtent(const State& s, GuestExtent size) { return {size.w / s.aa_x, size.h / s.aa_y}; }

// how each stage reads its texture, for the texel constants
struct StageReads {
    bool rt[4] = {};            // a render target
    float k[4] = {1, 1, 1, 1};  // Extension::Sample's k
    float wide[4] = {};         // host texels per console texel of a target read into a smaller one, 0 otherwise
    uint32_t size[4][2] = {};   // the bound target's console size when the texture is a smaller one on its memory
};

// D3D12_SAMPLER_DESC and D3D11_SAMPLER_DESC are the same layout
// replaced: the stage samples a title's replacement picture (FindReplacement), filtered across the levels made for it
D3D12_SAMPLER_DESC StageSampler(const State& s, int i, uint32_t mip_levels, uint8_t shadow, bool clamp, uint32_t aniso,
                                bool replaced = false);

uint32_t SimpleState(const State& s, uint32_t method);
DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count);
void EnsureProgramHash(const State& s);
uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh);
std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh);
void KeyBlend(const State& s, PsoKey& k);
void KeyDepthStencil(const State& s, const Assembled& a, float ds_scale, PsoKey& k);
void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k);
void BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, const PipelineTargets& t, PsoKey& k);

void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c);
void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c);
void FillCombinerConstants(const State& s, DrawConstants& c);
void FillTexelConstants(int i, const TextureDesc& d, const StageReads& stages, DrawConstants& c);
void FillTextureConstants(const State& s, const StageReads& stages, DrawConstants& c);
void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c);
void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c);

bool UnitTexcoords(const Assembled& a);
D3D_PRIMITIVE_TOPOLOGY PrimitiveTopology(const Assembled& a);

// Ultrawide: a screen shaped target shown wider than its guest shape gets more pixels across than down
inline float XStretch(float aspect, uint32_t guest_h, uint32_t gw, uint32_t gh) {
    if (!guest_h || gh != guest_h || gw < gh || aspect <= 0) return 1;
    return std::max(1.0f, aspect * float(gh) / float(gw));
}
// DrawConstants::hud[0] for a draw on a target stretched by stretch (0 without a colour target)
bool DepthTested(const State& s);
bool DepthOnlySampled(const State& s);  // the bound depth buffer is a texture of a draw that leaves it alone
bool ScreenDraw(const State& s);
float HudSqueeze(const State& s, const Assembled& a, float aspect, float stretch);
// a depth target sampled as format: through a depth format the console compares (a shadow map), through a 32 bit
// colour format it reads the word's bytes (PGR2's depth of field weighs its blur by them)
void DepthRead(uint32_t format, TexInfo& info);
// Ultrawide: the squeeze decided for a pretransformed draw, logged once for each target and outcome (rt the target's
// data, gw x gh its console size, w its host width): why a menu or HUD is or is not kept in the centre
void NoteSqueeze(const State& s, const Assembled& a, uint32_t rt, uint32_t gw, uint32_t gh, uint32_t w, float aspect,
                 float stretch, float squeeze);

// a guest rect scaled to the target and clamped to it (any target with w, h, gw, gh)
template <class T>
RECT ScaledRect(const T& t, int x0, int y0, int x1, int y1) {
    float sx = float(t.w) / t.gw, sy = float(t.h) / t.gh;
    return {std::clamp<LONG>(LONG(x0 * sx), 0, t.w), std::clamp<LONG>(LONG(y0 * sy), 0, t.h),
            std::clamp<LONG>(LONG(x1 * sx), 0, t.w), std::clamp<LONG>(LONG(y1 * sy), 0, t.h)};
}

// CopyRects' guest lists
struct GuestRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};
struct GuestPoint {
    int32_t x;
    int32_t y;
};
GuestRect ReadGuestRect(uint32_t rects, uint32_t i);
GuestPoint ReadGuestPoint(uint32_t points, uint32_t i);
// a drawn copy's rect in guest pixels: the source corners and where it lands
struct DrawnRect {
    float x0 = 0;
    float y0 = 0;
    float x1 = 0;
    float y1 = 0;
    float dx = 0;
    float dy = 0;
};
DrawnRect ReadDrawnRect(uint32_t src_rects, uint32_t dst_points, uint32_t i);
UINT ScaleUnsigned(int32_t v, float sc);  // a guest coordinate read as unsigned, times the host scale

}  // namespace gpu
