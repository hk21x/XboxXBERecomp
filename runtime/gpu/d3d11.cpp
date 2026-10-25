// The title's Direct3D 8 calls drawn with Direct3D 11 (renderer = "d3d11" in the settings file), for GPUs and drivers
// where Direct3D 12 runs badly. The console path of d3d12.cpp, with what does not depend on the API shared through
// d3d_common. Not here yet: F3 captures, renderer extensions and the perf report.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "core/diag.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/d3d_common.h"
#include "gpu/ext.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"

using Microsoft::WRL::ComPtr;

namespace gpu {

namespace {

constexpr UINT kBuffers = 2;
constexpr DXGI_FORMAT kBgra = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr uint64_t kStaleFrames = 8;          // frames a target may go unused and still be read back
constexpr uint32_t kVertexRing = 32u << 20;   // dynamic vertex buffer, appended to and started over when full
constexpr uint32_t kIndexRing = 8u << 20;
constexpr int kLateSlots = 3;                 // copies in flight per surface locked without a wait

// the shared enums and descriptions are D3D12's; D3D11's have the same values and layouts
static_assert(int(D3D11_BLEND_BLEND_FACTOR) == int(D3D12_BLEND_BLEND_FACTOR));
static_assert(int(D3D11_COMPARISON_ALWAYS) == int(D3D12_COMPARISON_FUNC_ALWAYS));
static_assert(int(D3D11_STENCIL_OP_DECR) == int(D3D12_STENCIL_OP_DECR));
static_assert(int(D3D11_TEXTURE_ADDRESS_MIRROR_ONCE) == int(D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE));
static_assert(int(D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT) == int(D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT));
static_assert(int(D3D11_FILTER_ANISOTROPIC) == int(D3D12_FILTER_ANISOTROPIC));
static_assert(sizeof(D3D11_SAMPLER_DESC) == sizeof(D3D12_SAMPLER_DESC));
static_assert(offsetof(D3D11_SAMPLER_DESC, MaxLOD) == offsetof(D3D12_SAMPLER_DESC, MaxLOD));

// a console surface drawn into
struct Target {
    ComPtr<ID3D11Texture2D> res;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11Texture2D> fb;  // copy read by draws that sample the target they render to
    ComPtr<ID3D11ShaderResourceView> fb_srv;
    bool depth = false;
    uint32_t gw = 0, gh = 0, w = 0, h = 0;
    uint32_t data = 0, bytes = 0;
    uint64_t epoch = 0, sig = 0;  // last lock seen, guest memory as last matched
    bool drawn = false;           // the GPU wrote it since the last readback
    bool linear32 = true;         // a 32 bit linear surface in guest memory, the only kind written back
    std::vector<uint8_t> read;    // what Sync left in guest memory
    // MSAA: a multisampled twin that draws go to, resolved into res before anything reads res
    ComPtr<ID3D11Texture2D> ms;
    ComPtr<ID3D11RenderTargetView> ms_rtv;
    ComPtr<ID3D11DepthStencilView> ms_dsv;
    ComPtr<ID3D11ShaderResourceView> ms_srv;  // a depth twin read by its resolve
    bool ms_dirty = false;  // the twin has pixels res has not
    bool ms_stale = false;  // res has pixels the twin has not
};

// a texture from guest memory
struct Tex {
    ComPtr<ID3D11Resource> res;
    ComPtr<ID3D11ShaderResourceView> srv;
    TextureDesc d;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    uint64_t palette = 0;  // hash of the colours a P8 texture was decoded with
    uint64_t faces = 0;    // a cube's frame its faces drawn on the GPU were last copied in
};

// a draw's four texture stages
struct DrawTextures {
    TexInfo info[4];
    ID3D11ShaderResourceView* srv[4] = {};
    uint32_t levels[4] = {1, 1, 1, 1};
    bool replaced[4] = {};  // a title's replacement picture (FindReplacement)
    StageReads reads;
};

// the state objects one PsoKey stands for
struct Pipeline {
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11RasterizerState> raster;
    bool points = false;
};

// Blit's settings past the source and its rectangle
struct BlitOptions {
    bool gamma = false;    // through the gamma lookup
    bool alpha = false;    // the source's alpha, else 1
    uint32_t taps = 1;     // n x n box filter per pixel
    bool over = false;     // premultiplied over the target, which keeps its alpha
    bool fxaa = false;     // fast approximate antialiasing (the finished frame)
};

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

// after a Sync every byte is compared: the signature samples a big surface and misses small writes (XFONT text)
bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

// a lock alone proves nothing, reads lock too: only changed bytes mean the CPU wrote it
bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = SurfaceSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

// the pixels the CPU changed since the last Sync, opaque, the rest 0; t.read takes them
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

// a smaller depth surface's console size, above the address bits
constexpr int kKeyWidthShift = 34;
constexpr int kKeyHeightShift = 48;

D3D11_TEXTURE2D_DESC Texture2DDesc(DXGI_FORMAT fmt, uint32_t w, uint32_t h, UINT bind) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    return td;
}

// bytes per block of a compressed host format, 0 for the 4 byte texel formats
uint32_t BlockBytes(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_BC1_UNORM: return 8;
        case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC3_UNORM: return 16;
        default: return 0;
    }
}

class D3D11Backend final : public Backend {
public:
    ~D3D11Backend() override {
        if (ui_) ImGui_ImplDX11_Shutdown();
    }

    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override {
        if (!DeliverLate(data)) Sync(data, 0);
        RecordLate(data);
    }
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    // the GPU finishes what was sent, so every report asked for so far is written (titles poll inside a frame)
    void Flush() override {
        ctx_->Flush();
        DeliverReports(true);
    }
    void SetGamma(const uint8_t* ramp) override {
        std::memcpy(ramp_, ramp, sizeof(ramp_));
        ramp_dirty_ = true;
    }
    void ClearReport() override { counted_.clear(); }
    void GetReport(uint32_t report) override { reports_.push_back({report, counted_}); }

private:
    struct Query {
        ComPtr<ID3D11Query> q;
        float samples_per_pixel;
    };
    struct Report {
        uint32_t va;
        std::vector<Query> queries;
    };
    // a surface locked without a wait: copies in flight, the newest finished one kept on the CPU for DeliverLate
    struct Late {
        ComPtr<ID3D11Texture2D> staging[kLateSlots];
        uint64_t frame[kLateSlots] = {};  // 0 free
        std::vector<uint8_t> cpu;         // rows packed
        uint64_t cpu_frame = 0, delivered = 0;
        uint32_t gw = 0, gh = 0, next = 0;
    };
    struct Ring {
        ComPtr<ID3D11Buffer> buf;
        uint32_t size = 0, used = 0;
    };

    // Init, in order
    bool CreateDevice();
    bool CreateSwapChain(HWND hwnd);
    void CreateBackBufferView();
    bool CreateFixed();
    // Present's steps
    void ResizeSwap();
    void UploadGammaRamp();
    void BlitFrame(Target& t);
    void RenderUi();
    void ReleaseOldTextures();
    void CountFps();
    void ApplySettings();
    void RescaleTargets();
    // targets
    uint64_t SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const;
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth, uint32_t gw = 0, uint32_t gh = 0);
    Target* DepthTarget(const State& s);
    bool CreateTarget(Target& t);
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    bool TargetStale(uint32_t data) const;
    bool BindTargets(const State& s);
    void Unbind() {
        bound_rt_ = bound_ds_ = nullptr;
        bound_ms_ = false;
        ForgetDrawState();
    }
    void UploadTarget(Target& t, const Surface& s);
    // MSAA twins (msaa_ samples): colour targets at least half the console's screen, and the depth drawn with them
    bool MsEligible(const Target& t) const { return msaa_ > 1 && (t.depth || (t.gw >= 320 && t.gh >= 240)); }
    bool EnsureMs(Target& t);
    void Settle(Target& t);   // the twin's pixels into res
    void Refresh(Target& t);  // res's pixels into the twin
    void Wrote(Target& t) {   // res written by other than a draw: the twin is behind
        if (t.ms) t.ms_stale = true, t.ms_dirty = false;
    }
    void DropMs(Target& t);
    ComPtr<ID3D11Texture2D> PrepareCopySource(Target& t);  // guest sized BGRA: the target, or a shrunk copy
    ComPtr<ID3D11Texture2D> Shrink(Target& t, uint32_t w, uint32_t h);
    bool ReadBack(ID3D11Texture2D* src, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
    // full screen passes
    ID3DBlob* Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash);
    ID3D11VertexShader* VertexShader(ID3DBlob* code, uint64_t hash);
    ID3D11PixelShader* PixelShader(ID3DBlob* code, uint64_t hash);
    void SetPassTarget(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, uint32_t w, uint32_t h);
    void FullScreen(ID3D11VertexShader* vs, ID3D11PixelShader* ps, const float* cb, uint32_t cb_floats,
                    ID3D11ShaderResourceView* const* srv, uint32_t n_srv, ID3D11BlendState* blend,
                    ID3D11DepthStencilState* depth, UINT stencil_ref = 0);
    void Blit(ID3D11ShaderResourceView* src, const float uvrect[4], const BlitOptions& o);
    bool DepthCopy(const State& s);
    void ClearRects(Target& t, const std::vector<RECT>& rs, uint32_t flags, const float colour[4], float z,
                    uint32_t stencil);
    void CopyByDraw(Target& src_t, Target& dst_t, uint32_t count, uint32_t src_rects, uint32_t dst_points);
    void CopyByRegion(Target& src_t, Target& dst_t, uint32_t count, uint32_t src_rects, uint32_t dst_points);
    // a draw's steps, in call order
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    ID3D11ShaderResourceView* BindTexture(const State& s, int stage, DrawTextures& stages);
    ID3D11ShaderResourceView* BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t,
                                                DrawTextures& stages);
    ID3D11ShaderResourceView* BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                               DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi);
    void UploadTexture(Tex& tx, const TextureDesc& d, const uint8_t* p, const uint32_t* colours);
    ID3D11ShaderResourceView* BindReplacement(const Replacement& r, const TextureDesc& d, uint32_t& levels);
    void CopyCubeTargets(Tex& tx, const TextureDesc& d);
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh);
    Pipeline* EnsurePipeline(const PsoKey& k, ID3DBlob* vs);
    void UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                         GuestExtent size, DrawConstants& c);
    void SetTexturesAndSamplers(const State& s, const Assembled& a, const DrawTextures& stages);
    void SetViewportScissor(const State& s, uint32_t gw);
    void DrawGeometry(const Assembled& a);
    uint32_t Reserve(Ring& r, uint32_t bytes);
    uint32_t Push(Ring& r, uint32_t at, const void* p, uint32_t bytes);
    void UpdateBuffer(ID3D11Buffer* b, const void* p, uint32_t bytes);
    // visibility tests
    ComPtr<ID3D11Query> NewQuery();
    float SamplesPerPixel() const;
    void DeliverReports(bool wait);

    // what the draw path set last, so equal state is not set again; anything else that sets it calls ForgetDrawState
    struct DrawStateCache {
        ID3D11VertexShader* vs = nullptr;
        ID3D11PixelShader* ps = nullptr;
        ID3D11GeometryShader* gs = nullptr;
        ID3D11InputLayout* layout = nullptr;
        ID3D11BlendState* blend = nullptr;
        ID3D11DepthStencilState* depth = nullptr;
        ID3D11RasterizerState* raster = nullptr;
        float bf[4] = {-1, -1, -1, -1};
        uint32_t sref = ~0u;
        int topo = -1;
        bool cb = false;   // the draw constant buffers bound
        bool set = false;  // shaders, layout and state objects as recorded here
        D3D11_VIEWPORT vp{};
        D3D11_RECT sc{};
        bool vp_ok = false;
    } ds_;
    void ForgetDrawState() { ds_ = {}; }

    ComPtr<IDXGIFactory2> factory_;
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<ID3D11RenderTargetView> bb_rtv_;
    // fixed objects: the passes' state, samplers, constant buffers, the gamma lookup, the rings
    ComPtr<ID3D11RasterizerState> pass_raster_;
    ComPtr<ID3D11DepthStencilState> no_depth_, depth_always_;
    ComPtr<ID3D11BlendState> opaque_, over_;
    ComPtr<ID3D11SamplerState> pass_samplers_[4];  // linear, then point; all clamp
    ComPtr<ID3D11Buffer> vsc_cb_, dc_cb_, pass_cb_;
    ComPtr<ID3D11Texture2D> lut_;
    ComPtr<ID3D11ShaderResourceView> lut_srv_;
    Ring vring_, iring_;
    std::vector<uint8_t> vsc_last_, dc_last_;
    // caches
    std::unordered_map<uint64_t, ComPtr<ID3DBlob>> blobs_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;  // vertex and pixel shader keys to source hash
    std::unordered_map<uint64_t, ComPtr<ID3D11VertexShader>> vs_;
    std::unordered_map<uint64_t, ComPtr<ID3D11PixelShader>> ps_;
    ComPtr<ID3D11GeometryShader> point_gs_;
    std::unordered_map<uint64_t, Pipeline> pipelines_;
    std::unordered_map<uint64_t, ComPtr<ID3D11SamplerState>> samplers_;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint32_t, uint64_t> target_used_;  // target address to the frame it was last used
    std::unordered_map<uint64_t, Tex> textures_;          // by data address, a P8 texture's with its palette's above
    std::unordered_map<uint32_t, uint32_t> reloads_;      // targets reloaded from guest memory, for the log
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;                    // DeliverLate runs on the title's thread
    std::atomic<uint64_t> presents_seen_{0};  // presents_ for DeliverLate
    std::vector<ComPtr<ID3D11Query>> free_queries_;
    std::vector<Query> counted_;  // since the last ClearReport
    std::deque<Report> reports_;
    Assembled asm_;  // kept between draws for its buffers
    Target* bound_rt_ = nullptr;
    Target* bound_ds_ = nullptr;
    bool bound_ms_ = false;  // the bound targets are twins
    uint32_t msaa_ = 1;
    Target* draw_rt_ = nullptr;  // the draw's targets, the bound ones go when a texture's copy is drawn
    Target* draw_ds_ = nullptr;
    uint32_t guest_w_ = 640, guest_h_ = 480;
    uint32_t bb_w_ = 0, bb_h_ = 0;
    float scale_ = 1;
    float aspect_ = 0;  // app::FrameAspect() the targets were made for
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = false;
    bool gamma_ = false;
    bool tearing_ = false;
    bool ui_ = false;
    uint64_t presents_ = 0;
    double fps_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

// ---- device, swap chain, fixed objects

bool D3D11Backend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    aspect_ = app::FrameAspect();
    scale_ = app::RenderScale(guest_h_);
    HWND hwnd = static_cast<HWND>(app::Window());
    if (!CreateDevice() || !CreateSwapChain(hwnd) || !CreateFixed()) return false;
    ui_ = ImGui_ImplDX11_Init(dev_.Get(), ctx_.Get());
    if (g_extension) LOGW("d3d11: the renderer extension is not used under Direct3D 11");
    LOGR("d3d11: drawing renderer, %ux%u guest, %ux%u window, render scale %.3f", width, height, bb_w_, bb_h_, scale_);
    return true;
}

bool D3D11Backend::CreateDevice() {
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)))) return false;
    // dedicated GPU first on hybrid laptops
    ComPtr<IDXGIFactory6> f6;
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(factory_.As(&f6)))
        f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    UINT flags = GetEnvironmentVariableW(L"XBE_D3D11_DEBUG", nullptr, 0) ? D3D11_CREATE_DEVICE_DEBUG : 0;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    auto create = [&](IDXGIAdapter* a) {
        return SUCCEEDED(D3D11CreateDevice(a, a ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                           levels, UINT(std::size(levels)), D3D11_SDK_VERSION, &dev_, nullptr, &ctx_));
    };
    if (!create(adapter.Get()) && !create((adapter = nullptr).Get())) {
        LOGE("d3d11: no device");
        return false;
    }
    DXGI_ADAPTER_DESC1 ad{};
    if (adapter) adapter->GetDesc1(&ad);
    LOGR("D3D11 adapter: %ls, feature level %X", adapter ? ad.Description : L"default", unsigned(dev_->GetFeatureLevel()));
    ComPtr<IDXGIFactory5> f5;
    BOOL tear = FALSE;
    if (SUCCEEDED(factory_.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof(tear));
    tearing_ = tear;
    return true;
}

bool D3D11Backend::CreateSwapChain(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    bb_w_ = std::max<LONG>(rc.right - rc.left, 1);
    bb_h_ = std::max<LONG>(rc.bottom - rc.top, 1);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = bb_w_;
    sd.Height = bb_h_;
    sd.Format = kBgra;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBuffers;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(factory_->CreateSwapChainForHwnd(dev_.Get(), hwnd, &sd, nullptr, nullptr, &swap_))) {
        LOGE("d3d11: no swap chain");
        return false;
    }
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    CreateBackBufferView();
    return true;
}

void D3D11Backend::CreateBackBufferView() {
    ComPtr<ID3D11Texture2D> bb;
    swap_->GetBuffer(0, IID_PPV_ARGS(&bb));
    dev_->CreateRenderTargetView(bb.Get(), nullptr, &bb_rtv_);
}

bool D3D11Backend::CreateFixed() {
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    dev_->CreateRasterizerState(&rd, &pass_raster_);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dev_->CreateDepthStencilState(&dd, &no_depth_);
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dev_->CreateDepthStencilState(&dd, &depth_always_);
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev_->CreateBlendState(&bd, &opaque_);
    // premultiplied colour, the target keeps its alpha
    auto& b = bd.RenderTarget[0];
    b.BlendEnable = TRUE;
    b.SrcBlend = D3D11_BLEND_ONE;
    b.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    b.SrcBlendAlpha = D3D11_BLEND_ZERO;
    b.DestBlendAlpha = D3D11_BLEND_ONE;
    b.BlendOp = b.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    dev_->CreateBlendState(&bd, &over_);
    for (int i = 0; i < 4; i++) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = i == 0 ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev_->CreateSamplerState(&sd, &pass_samplers_[i]);
    }
    auto cbuffer = [&](uint32_t bytes, ComPtr<ID3D11Buffer>& out) {
        D3D11_BUFFER_DESC d{(bytes + 15) & ~15u, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
        return SUCCEEDED(dev_->CreateBuffer(&d, nullptr, &out));
    };
    auto ring = [&](uint32_t bytes, UINT bind, Ring& out) {
        D3D11_BUFFER_DESC d{bytes, D3D11_USAGE_DYNAMIC, bind, D3D11_CPU_ACCESS_WRITE};
        out.size = bytes;
        out.used = bytes;  // the first Reserve starts it over
        return SUCCEEDED(dev_->CreateBuffer(&d, nullptr, &out.buf));
    };
    if (!cbuffer(sizeof(State::vsc), vsc_cb_) || !cbuffer(sizeof(DrawConstants), dc_cb_) || !cbuffer(256, pass_cb_) ||
        !ring(kVertexRing, D3D11_BIND_VERTEX_BUFFER, vring_) || !ring(kIndexRing, D3D11_BIND_INDEX_BUFFER, iring_)) {
        LOGE("d3d11: buffers could not be made");
        return false;
    }
    // the gamma lookup: one BGRA texel per ramp entry
    D3D11_TEXTURE2D_DESC td = Texture2DDesc(kBgra, 256, 1, D3D11_BIND_SHADER_RESOURCE);
    dev_->CreateTexture2D(&td, nullptr, &lut_);
    dev_->CreateShaderResourceView(lut_.Get(), nullptr, &lut_srv_);
    return true;
}

// ---- targets

// a smaller depth surface laid on the depth buffer's memory is a target of its own
uint64_t D3D11Backend::SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const {
    uint64_t key = TargetMapKey(data, depth);
    bool small_depth = depth && (gw != guest_w_ || gh != guest_h_);
    if (!small_depth) return key;
    return key | uint64_t(gw) << kKeyWidthShift | uint64_t(gh) << kKeyHeightShift;
}

// the surface's target at its size, null when there is none
Target* D3D11Backend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data);
    uint32_t gw, gh;
    SurfaceSize(s, gw, gh);
    auto it = targets_.find(SizedTargetKey(data, depth, gw, gh));
    if (it == targets_.end() || it->second.gw != gw || it->second.gh != gh) return nullptr;
    target_used_[data] = presents_;
    return &it->second;
}

// the host texture and views of a new target, at render scale
bool D3D11Backend::CreateTarget(Target& t) {
    t.w = std::max(1u, uint32_t(std::lround(t.gw * scale_ * XStretch(aspect_, guest_h_, t.gw, t.gh))));
    t.h = std::max(1u, uint32_t(std::lround(t.gh * scale_)));
    if (t.depth) {
        D3D11_TEXTURE2D_DESC td = Texture2DDesc(DXGI_FORMAT_R24G8_TYPELESS, t.w, t.h,
                                                D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &t.res))) return false;
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        if (FAILED(dev_->CreateDepthStencilView(t.res.Get(), &dv, &t.dsv)) ||
            FAILED(dev_->CreateShaderResourceView(t.res.Get(), &sv, &t.srv)))
            return false;
        ctx_->ClearDepthStencilView(t.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
    } else {
        D3D11_TEXTURE2D_DESC td = Texture2DDesc(kBgra, t.w, t.h, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &t.res)) ||
            FAILED(dev_->CreateRenderTargetView(t.res.Get(), nullptr, &t.rtv)) ||
            FAILED(dev_->CreateShaderResourceView(t.res.Get(), nullptr, &t.srv)))
            return false;
        float black[4] = {0, 0, 0, 0};
        ctx_->ClearRenderTargetView(t.rtv.Get(), black);
    }
    return true;
}

void D3D11Backend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    if (bound_rt_ == &it->second || bound_ds_ == &it->second) Unbind();
    targets_.erase(it);
}

// the surface's target, made again at its size when that changed; gw and gh, when set, stand in for the surface's own
Target* D3D11Backend::EnsureTarget(const Surface& s, bool depth, uint32_t gw, uint32_t gh) {
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
        LOG_FIRST(8, "d3d11: target %08X %ux%u could not be made", data, sw, sh);
        return nullptr;
    }
    t.data = data;
    TextureDesc d;
    t.linear32 = depth || (Describe(s.header, d) && d.linear && Format(d.format).bits == 32);
    t.bytes = depth ? sw * sh * 4 : GuestPitch(sw) * sh;
    t.epoch = DirtyEpoch(data);
    t.sig = SurfaceSig(data, t.bytes);
    return &(targets_[key] = std::move(t));
}

// a smaller colour target drawn with a bigger depth buffer uses its top left part: a depth target of the colour
// target's size stands in (d3d12.cpp DepthTarget)
Target* D3D11Backend::DepthTarget(const State& s) {
    uint32_t rw = 0, rh = 0, zw = 0, zh = 0;
    if (s.rt.header && s.zs.header) {
        SurfaceSize(s.rt, rw, rh);
        SurfaceSize(s.zs, zw, zh);
        if (rw <= zw && rh <= zh && (rw != zw || rh != zh)) return EnsureTarget(s.zs, true, rw, rh);
    }
    return EnsureTarget(s.zs, true);
}

// long unused: the title freed it and other data is at this address now
bool D3D11Backend::TargetStale(uint32_t data) const {
    auto it = target_used_.find(data);
    uint64_t used = it == target_used_.end() ? 0 : it->second;
    return presents_ - used > kStaleFrames;
}

bool D3D11Backend::BindTargets(const State& s) {
    bool depth = (s.rs[kRsZEnable] || s.rs[kRsStencilEnable]) && !DepthOnlySampled(s);
    // the colour target on the depth buffer's own memory: one resource cannot be bound as both here, so what the
    // console writes there alone. 007: Everything or Nothing draws its character shadows into a D24S8 surface given
    // as both with colour writes off (the depth target), and copies the frame into a colour surface on the same
    // memory for its weapon carousel, depth always passing and not written (the colour target)
    bool shared = s.rt.header && s.zs.header && s.rt.data == s.zs.data;
    bool colour = SimpleState(s, kMColorWriteEnable) != 0;
    Target* rt = shared && depth && !colour ? nullptr : EnsureTarget(s.rt, false);
    Target* ds = depth && !(shared && colour) ? DepthTarget(s) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return false;
    if (rt && LatchCpuWrite(*rt)) {
        if (++reloads_[rt->data] <= 3)
            LOGR("d3d11: target %08X %ux%u reloaded from guest memory before drawing (frame %llu)", rt->data, rt->gw,
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
    if (rt != bound_rt_ || ds != bound_ds_ || ms != bound_ms_) {
        ID3D11RenderTargetView* rtv = rt ? (ms ? rt->ms_rtv : rt->rtv).Get() : nullptr;
        ID3D11DepthStencilView* dsv = ds ? (ms ? ds->ms_dsv : ds->dsv).Get() : nullptr;
        ctx_->OMSetRenderTargets(rt ? 1 : 0, rt ? &rtv : nullptr, dsv);
        bound_rt_ = rt;
        bound_ds_ = ds;
        bound_ms_ = ms;
    }
    return true;
}

// CPU pixels of a colour surface into its target, scaled
void D3D11Backend::UploadTarget(Target& t, const Surface& s) {
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    // after a Sync only the pixels the CPU changed go over the picture, which stays at render scale
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    if (px.size() < size_t(pitch) * d.height) px.resize(size_t(pitch) * d.height);
    D3D11_TEXTURE2D_DESC td = Texture2DDesc(kBgra, d.width, d.height, D3D11_BIND_SHADER_RESOURCE);
    td.Usage = D3D11_USAGE_IMMUTABLE;
    D3D11_SUBRESOURCE_DATA init{px.data(), pitch, 0};
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(dev_->CreateTexture2D(&td, &init, &tex)) || FAILED(dev_->CreateShaderResourceView(tex.Get(), nullptr, &srv)))
        return;
    Settle(t);
    Wrote(t);
    SetPassTarget(t.rtv.Get(), nullptr, t.w, t.h);
    float uv[4] = {0, 0, 1, 1};
    Blit(srv.Get(), uv, {.alpha = true, .over = over});
    t.epoch = DirtyEpoch(t.data);
    t.sig = SurfaceSig(t.data, t.bytes);
}

// filters the target into a new w x h BGRA texture
ComPtr<ID3D11Texture2D> D3D11Backend::Shrink(Target& t, uint32_t w, uint32_t h) {
    D3D11_TEXTURE2D_DESC td = Texture2DDesc(kBgra, w, h, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    ComPtr<ID3D11Texture2D> down;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &down)) || FAILED(dev_->CreateRenderTargetView(down.Get(), nullptr, &rtv)))
        return nullptr;
    SetPassTarget(rtv.Get(), nullptr, w, h);
    float uv[4] = {0, 0, 1, 1};
    Blit(t.srv.Get(), uv, {.alpha = true});
    return down;
}

ComPtr<ID3D11Texture2D> D3D11Backend::PrepareCopySource(Target& t) {
    Settle(t);
    if (t.w != t.gw || t.h != t.gh) return Shrink(t, t.gw, t.gh);
    return t.res;
}

// a BGRA texture to the CPU, rows packed; waits for the GPU
bool D3D11Backend::ReadBack(ID3D11Texture2D* src, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    D3D11_TEXTURE2D_DESC td = Texture2DDesc(kBgra, w, h, 0);
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &st))) return false;
    ctx_->CopyResource(st.Get(), src);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    out.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++)
        std::memcpy(&out[size_t(y) * w * 4], static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, w * 4);
    ctx_->Unmap(st.Get(), 0);
    return true;
}

// the CPU reads a surface the GPU drew: bring the pixels back
void D3D11Backend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn || TargetStale(data)) return;
    Target& t = it->second;
    // pixels written back are 32 bit linear rows: over a narrower or swizzled surface they would run past it
    if (!t.linear32) {
        LOG_FIRST(8, "d3d11: read-back of %08X skipped, not a 32 bit linear surface", data);
        return;
    }
    ComPtr<ID3D11Texture2D> src = PrepareCopySource(t);
    std::vector<uint8_t> px;
    if (!src || !ReadBack(src.Get(), t.gw, t.gh, px)) return;
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = SurfaceSig(t.data, t.bytes);
}

// A NOOVERWRITE lock: the console reads whatever the GPU has finished, a frame or two old. RecordLate (in order with
// the draws) copies the surface into a staging texture and collects earlier copies the GPU has finished without
// waiting; DeliverLate (any thread) writes the newest collected one to guest memory.
bool D3D11Backend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end() || it->second.cpu.empty()) return false;
    Late& l = it->second;
    if (presents_seen_.load() - l.cpu_frame > uint64_t(kLateSlots) + 1) return false;
    if (l.cpu_frame > l.delivered) {
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) std::memcpy(dst + size_t(y) * pitch, &l.cpu[size_t(y) * l.gw * 4], l.gw * 4);
        l.delivered = l.cpu_frame;
    }
    return true;
}

void D3D11Backend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || TargetStale(data) || !it->second.linear32) return;
    Target& t = it->second;
    // what DeliverLate or Sync left in guest memory is no CPU write to upload back
    t.sig = SurfaceSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) {
        l = Late{};
        l.gw = t.gw;
        l.gh = t.gh;
    }
    // finished copies to the CPU, the newest kept
    for (int i = 0; i < kLateSlots; i++) {
        if (!l.frame[i]) continue;
        D3D11_MAPPED_SUBRESOURCE m;
        if (ctx_->Map(l.staging[i].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) continue;
        if (l.frame[i] >= l.cpu_frame) {
            l.cpu.resize(size_t(l.gw) * l.gh * 4);
            for (uint32_t y = 0; y < l.gh; y++)
                std::memcpy(&l.cpu[size_t(y) * l.gw * 4], static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, l.gw * 4);
            l.cpu_frame = l.frame[i];
        }
        ctx_->Unmap(l.staging[i].Get(), 0);
        l.frame[i] = 0;
    }
    // a copy for a later lock, unless every slot is still in flight
    uint32_t slot = l.next;
    if (l.frame[slot]) return;
    if (!l.staging[slot]) {
        D3D11_TEXTURE2D_DESC td = Texture2DDesc(kBgra, l.gw, l.gh, 0);
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &l.staging[slot]))) return;
    }
    ComPtr<ID3D11Texture2D> src = PrepareCopySource(t);
    if (!src) return;
    ctx_->CopyResource(l.staging[slot].Get(), src.Get());
    l.frame[slot] = presents_ + 1;
    l.next = (slot + 1) % kLateSlots;
}

// t's twin, made on first need; false when it can't be (the target is drawn without MSAA)
bool D3D11Backend::EnsureMs(Target& t) {
    if (t.ms) return true;
    DXGI_FORMAT fmt = t.depth ? DXGI_FORMAT_D24_UNORM_S8_UINT : kBgra;
    UINT quality = 0;
    if (FAILED(dev_->CheckMultisampleQualityLevels(fmt, msaa_, &quality)) || !quality) return false;
    D3D11_TEXTURE2D_DESC td = Texture2DDesc(t.depth ? DXGI_FORMAT_R24G8_TYPELESS : kBgra, t.w, t.h,
                                            t.depth ? D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE
                                                    : D3D11_BIND_RENDER_TARGET);
    td.SampleDesc.Count = msaa_;
    bool made = SUCCEEDED(dev_->CreateTexture2D(&td, nullptr, &t.ms));
    if (made && t.depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = fmt;
        dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        made = SUCCEEDED(dev_->CreateDepthStencilView(t.ms.Get(), &dv, &t.ms_dsv)) &&
               SUCCEEDED(dev_->CreateShaderResourceView(t.ms.Get(), &sv, &t.ms_srv));
    } else if (made) {
        D3D11_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = fmt;
        rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
        made = SUCCEEDED(dev_->CreateRenderTargetView(t.ms.Get(), &rv, &t.ms_rtv));
    }
    if (!made) {
        LOG_FIRST(4, "d3d11: MSAA twin of %08X %ux%u could not be made", t.data, t.w, t.h);
        t.ms.Reset();
        t.ms_rtv.Reset();
        t.ms_dsv.Reset();
        t.ms_srv.Reset();
        return false;
    }
    t.ms_dirty = false;
    t.ms_stale = true;  // filled from res before its first draw
    LOG_FIRST(8, "d3d11: MSAA %ux twin for %s %08X %ux%u", msaa_, t.depth ? "depth" : "target", t.data, t.w, t.h);
    return true;
}

// colour by the hardware resolve; depth by a pass writing each pixel's first sample
void D3D11Backend::Settle(Target& t) {
    if (!t.ms || !t.ms_dirty) return;
    t.ms_dirty = false;
    if (!t.depth) {
        ctx_->ResolveSubresource(t.res.Get(), 0, t.ms.Get(), 0, kBgra);
        return;
    }
    uint64_t h;
    ID3D11VertexShader* vs = VertexShader(Compile(kDepthResolveHlsl, "vs", "vs_5_0", h), h);
    ID3D11PixelShader* ps = PixelShader(Compile(kDepthResolveHlsl, "psmain", "ps_5_0", h), h);
    SetPassTarget(nullptr, t.dsv.Get(), t.w, t.h);
    ID3D11ShaderResourceView* srv = t.ms_srv.Get();
    FullScreen(vs, ps, nullptr, 0, &srv, 1, opaque_.Get(), depth_always_.Get());
}

void D3D11Backend::Refresh(Target& t) {
    if (!t.ms || !t.ms_stale) return;
    t.ms_stale = false;
    float uv[4] = {0, 0, 1, 1};
    if (!t.depth) {
        SetPassTarget(t.ms_rtv.Get(), nullptr, t.w, t.h);
        Blit(t.srv.Get(), uv, {.alpha = true});
        return;
    }
    uint64_t h;
    ID3D11VertexShader* vs = VertexShader(Compile(kDepthHlsl, "vs", "vs_5_0", h), h);
    ID3D11PixelShader* ps = PixelShader(Compile(kDepthHlsl, "psmain", "ps_5_0", h), h);
    SetPassTarget(nullptr, t.ms_dsv.Get(), t.w, t.h);
    ID3D11ShaderResourceView* srv = t.srv.Get();
    FullScreen(vs, ps, nullptr, 0, &srv, 1, opaque_.Get(), depth_always_.Get());
}

// the twin goes, its pixels settled into res first
void D3D11Backend::DropMs(Target& t) {
    if (!t.ms) return;
    Settle(t);
    t.ms.Reset();
    t.ms_rtv.Reset();
    t.ms_dsv.Reset();
    t.ms_srv.Reset();
    t.ms_dirty = t.ms_stale = false;
}

// ---- full screen passes

ID3DBlob* D3D11Backend::Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash) {
    // one source can hold several entry points
    uint64_t profile_seed = Hash(profile, std::strlen(profile));
    uint64_t entry_seed = Hash(entry, std::strlen(entry), profile_seed);
    hash = Hash(src.data(), src.size(), entry_seed);
    auto it = blobs_.find(hash);
    if (it != blobs_.end()) return it->second.Get();
    ComPtr<ID3DBlob> code, err;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, profile,
                            D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (FAILED(hr)) {
        LOGE("d3d11: %s compile failed: %s", profile, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        LOGE("%s", src.c_str());
        blobs_[hash] = nullptr;
        return nullptr;
    }
    blobs_[hash] = code;
    return code.Get();
}

ID3D11VertexShader* D3D11Backend::VertexShader(ID3DBlob* code, uint64_t hash) {
    if (!code) return nullptr;
    auto& v = vs_[hash];
    if (!v) dev_->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &v);
    return v.Get();
}

ID3D11PixelShader* D3D11Backend::PixelShader(ID3DBlob* code, uint64_t hash) {
    if (!code) return nullptr;
    auto& p = ps_[hash];
    if (!p) dev_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &p);
    return p.Get();
}

// one colour or depth target for a pass, the draw's bound targets forgotten
void D3D11Backend::SetPassTarget(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, uint32_t w, uint32_t h) {
    // views a draw left bound may be this target: they go first, or the runtime drops the new binding
    ID3D11ShaderResourceView* none[4] = {};
    ctx_->PSSetShaderResources(0, 4, none);
    ctx_->OMSetRenderTargets(rtv ? 1 : 0, rtv ? &rtv : nullptr, dsv);
    D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    D3D11_RECT sc{0, 0, LONG(w), LONG(h)};
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetScissorRects(1, &sc);
    Unbind();
}

// the full screen triangle with cb at b0 and b1, srv from t0, the pass samplers
void D3D11Backend::FullScreen(ID3D11VertexShader* vs, ID3D11PixelShader* ps, const float* cb, uint32_t cb_floats,
                              ID3D11ShaderResourceView* const* srv, uint32_t n_srv, ID3D11BlendState* blend,
                              ID3D11DepthStencilState* depth, UINT stencil_ref) {
    if (!vs || !ps) return;
    if (cb) UpdateBuffer(pass_cb_.Get(), cb, cb_floats * sizeof(float));
    ID3D11Buffer* cbs[2] = {pass_cb_.Get(), pass_cb_.Get()};
    ctx_->VSSetConstantBuffers(0, 2, cbs);
    ctx_->PSSetConstantBuffers(0, 2, cbs);
    ctx_->VSSetShader(vs, nullptr, 0);
    ctx_->PSSetShader(ps, nullptr, 0);
    ctx_->GSSetShader(nullptr, nullptr, 0);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->RSSetState(pass_raster_.Get());
    float zero[4] = {};
    ctx_->OMSetBlendState(blend, zero, 0xFFFFFFFF);
    ctx_->OMSetDepthStencilState(depth, stencil_ref);
    ID3D11SamplerState* smp[4] = {pass_samplers_[0].Get(), pass_samplers_[1].Get(), pass_samplers_[2].Get(),
                                  pass_samplers_[3].Get()};
    ctx_->PSSetSamplers(0, 4, smp);
    if (n_srv) ctx_->PSSetShaderResources(0, n_srv, srv);
    ctx_->Draw(3, 0);
    ID3D11ShaderResourceView* none[4] = {};
    if (n_srv) ctx_->PSSetShaderResources(0, n_srv, none);
    ForgetDrawState();
}

// src over the bound viewport and scissor
void D3D11Backend::Blit(ID3D11ShaderResourceView* src, const float uvrect[4], const BlitOptions& o) {
    uint64_t h;
    ID3D11VertexShader* vs = VertexShader(Compile(kBlitHlsl, "vs", "vs_5_0", h), h);
    ID3D11PixelShader* ps = PixelShader(Compile(kBlitHlsl, o.fxaa ? "psfxaa" : "psmain", "ps_5_0", h), h);
    if (!ps && o.fxaa) ps = PixelShader(Compile(kBlitHlsl, "psmain", "ps_5_0", h), h);  // a shader that failed: plain
    float c[8] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3], o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f,
                  float(o.taps), 1.0f};
    ID3D11ShaderResourceView* srv[2] = {src, lut_srv_.Get()};
    FullScreen(vs, ps, c, 8, srv, 2, o.over ? over_.Get() : opaque_.Get(), no_depth_.Get());
}

// a depth buffer drawn as colour into another depth buffer's memory: copied as depth
bool D3D11Backend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube ||
        d.depth != 1)
        return false;
    uint32_t gw, gh;
    SurfaceSize(s.rt, gw, gh);
    auto di = targets_.find(SizedTargetKey(mem::VirtualFromPhysical(s.rt.data), true, gw, gh));
    auto si = targets_.find(SizedTargetKey(d.data, true, d.width, d.height));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;
    Target& dst = di->second;
    Target& src = si->second;
    uint64_t h;
    ID3D11VertexShader* vs = VertexShader(Compile(kDepthHlsl, "vs", "vs_5_0", h), h);
    ID3D11PixelShader* ps = PixelShader(Compile(kDepthHlsl, "psmain", "ps_5_0", h), h);
    if (!vs || !ps) return false;
    Settle(src);
    Settle(dst);
    Wrote(dst);
    SetPassTarget(nullptr, dst.dsv.Get(), dst.w, dst.h);
    ID3D11ShaderResourceView* srv = src.srv.Get();
    FullScreen(vs, ps, nullptr, 0, &srv, 1, opaque_.Get(), depth_always_.Get());
    dst.drawn = true;
    return true;
}

// ---- clears

// a clear of part of a target: the full screen triangle in each rect
void D3D11Backend::ClearRects(Target& t, const std::vector<RECT>& rs, uint32_t flags, const float colour[4], float z,
                              uint32_t stencil) {
    uint64_t h;
    ID3D11VertexShader* vs = VertexShader(Compile(kClearHlsl, "vs", "vs_5_0", h), h);
    ID3D11PixelShader* ps = PixelShader(Compile(kClearHlsl, t.depth ? "psdepth" : "pscolour", "ps_5_0", h), h);
    ComPtr<ID3D11DepthStencilState> dss = no_depth_;
    if (t.depth) {
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = (flags & kClearZ) != 0;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dd.StencilEnable = (flags & kClearStencil) != 0;
        dd.StencilReadMask = dd.StencilWriteMask = 0xFF;
        dd.FrontFace = dd.BackFace = {D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE,
                                      D3D11_COMPARISON_ALWAYS};
        dev_->CreateDepthStencilState(&dd, &dss);
    }
    SetPassTarget(t.depth ? nullptr : t.rtv.Get(), t.depth ? t.dsv.Get() : nullptr, t.w, t.h);
    float cb[8] = {colour[0], colour[1], colour[2], colour[3], z, 0, 0, 0};
    for (const RECT& r : rs) {
        if (r.right <= r.left || r.bottom <= r.top) continue;
        ctx_->RSSetScissorRects(1, &r);
        FullScreen(vs, ps, cb, 8, nullptr, 0, opaque_.Get(), dss.Get(), stencil & 0xFF);
    }
    Unbind();
}

void D3D11Backend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
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
        bool whole = rs.size() == 1 && rs[0].left == 0 && rs[0].top == 0 && rs[0].right == LONG(t->w) &&
                     rs[0].bottom == LONG(t->h);
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (!t->depth && LatchCpuWrite(*t)) UploadTarget(*t, s.rt);
        // a whole clear goes to the twin too and leaves both alike; part of one goes to res, the twin refreshed later
        if (!whole) Settle(*t);
        if (!t->depth) {
            if (!whole) ClearRects(*t, rs, flags, col, z, stencil);
            else ctx_->ClearRenderTargetView(t->rtv.Get(), col);
            if (whole && t->ms) ctx_->ClearRenderTargetView(t->ms_rtv.Get(), col);
        } else if (whole) {
            UINT cf = ((flags & kClearZ) ? D3D11_CLEAR_DEPTH : 0) | ((flags & kClearStencil) ? D3D11_CLEAR_STENCIL : 0);
            ctx_->ClearDepthStencilView(t->dsv.Get(), cf, std::clamp(z, 0.0f, 1.0f), UINT8(stencil));
            if (t->ms) ctx_->ClearDepthStencilView(t->ms_dsv.Get(), cf, std::clamp(z, 0.0f, 1.0f), UINT8(stencil));
        } else {
            ClearRects(*t, rs, flags, col, std::clamp(z, 0.0f, 1.0f), stencil);
        }
        if (whole && t->ms) t->ms_dirty = t->ms_stale = false;
        else Wrote(*t);
        t->drawn = true;
    }
}

// ---- copies

// one Blit per rect: the targets' scales differ
void D3D11Backend::CopyByDraw(Target& src_t, Target& dst_t, uint32_t count, uint32_t src_rects, uint32_t dst_points) {
    Settle(dst_t);
    Wrote(dst_t);
    SetPassTarget(dst_t.rtv.Get(), nullptr, dst_t.w, dst_t.h);
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
        D3D11_VIEWPORT vp{r.dx * dst_scale, r.dy * dst_scale_y, (r.x1 - r.x0) * dst_scale, (r.y1 - r.y0) * dst_scale_y, 0, 1};
        LONG right = std::min(LONG(vp.TopLeftX + vp.Width), LONG(dst_t.w));
        LONG bottom = std::min(LONG(vp.TopLeftY + vp.Height), LONG(dst_t.h));
        D3D11_RECT sc{LONG(vp.TopLeftX), LONG(vp.TopLeftY), right, bottom};
        if (sc.right <= sc.left || sc.bottom <= sc.top) continue;
        ctx_->RSSetViewports(1, &vp);
        ctx_->RSSetScissorRects(1, &sc);
        float uv[4] = {r.x0 / src_t.gw, r.y0 / src_t.gh, (r.x1 - r.x0) / src_t.gw, (r.y1 - r.y0) / src_t.gh};
        uint32_t taps = uint32_t(std::ceil(std::max(src_scale / dst_scale, 1.0f) - kTapsSlack));
        Blit(src_t.srv.Get(), uv, {.alpha = true, .taps = taps});
    }
    dst_t.drawn = true;
    Unbind();
}

// one CopySubresourceRegion per rect, clamped to both targets
void D3D11Backend::CopyByRegion(Target& src_t, Target& dst_t, uint32_t count, uint32_t src_rects, uint32_t dst_points) {
    Settle(dst_t);
    Wrote(dst_t);
    float dst_scale = float(dst_t.w) / dst_t.gw;
    float dst_scale_y = float(dst_t.h) / dst_t.gh;
    bool listed = count && src_rects;
    uint32_t n = listed ? count : 1;
    for (uint32_t i = 0; i < n; i++) {
        D3D11_BOX b{0, 0, 0, std::min(src_t.w, dst_t.w), std::min(src_t.h, dst_t.h), 1};
        uint32_t dx = 0, dy = 0;
        if (listed) {
            GuestRect r = ReadGuestRect(src_rects, i);
            b.left = ScaleUnsigned(r.left, dst_scale);
            b.top = ScaleUnsigned(r.top, dst_scale_y);
            b.right = std::min(ScaleUnsigned(r.right, dst_scale), src_t.w);
            b.bottom = std::min(ScaleUnsigned(r.bottom, dst_scale_y), src_t.h);
            dx = b.left;
            dy = b.top;
            if (dst_points) {
                GuestPoint p = ReadGuestPoint(dst_points, i);
                dx = ScaleUnsigned(p.x, dst_scale);
                dy = ScaleUnsigned(p.y, dst_scale_y);
            }
        }
        if (b.right <= b.left || b.bottom <= b.top || dx >= dst_t.w || dy >= dst_t.h) continue;
        b.right = std::min<UINT>(b.right, b.left + dst_t.w - dx);
        b.bottom = std::min<UINT>(b.bottom, b.top + dst_t.h - dy);
        ctx_->CopySubresourceRegion(dst_t.res.Get(), 0, dx, dy, 0, src_t.res.Get(), 0, &b);
    }
    dst_t.drawn = true;
}

void D3D11Backend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) {
    Target* src_t = FindTarget(src, false);
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "d3d11: CopyRects %08X to %08X skipped", src.data, dst.data);
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

ID3D11ShaderResourceView* D3D11Backend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t,
                                                          DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (&t == draw_ds_) return nullptr;
    Settle(t);
    stages.reads.rt[stage] = true;
    if (!t.depth && LatchCpuWrite(t)) {
        if (++reloads_[t.data] <= 3)
            LOGR("d3d11: sampled target %08X %ux%u reloaded from guest memory (frame %llu)", t.data, t.gw, t.gh,
                 (unsigned long long)presents_);
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    // a pass shrinking it averages each tap over a console texel, else upscaled taps skip texels and ghost
    Target* rt = FindTarget(s.rt, false);
    float k = float(t.w) / t.gw;
    if (!t.depth && k > kUpscaledMin && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.reads.wide[stage] = k;
        info.flags |= kTexWide;
    }
    if (NoAlphaFormat(d.format)) info.flags |= kTexOpaque;  // also a depth buffer read as X8R8G8B8
    // the console draws an 8 bit surface as its blue byte; read as L8, AL8 or A8 the D3D12 renderer spreads that byte
    // with the view's swizzle, which D3D11 views lack
    if (!t.depth) {
        switch (d.format) {
            case 0x00: case 0x13: case 0x01: case 0x1B: case 0x19: case 0x1F:
                LOG_FIRST(4, "d3d11: target %08X sampled as 8 bit format %X, read as BGRA", t.data, d.format);
                break;
            default: break;
        }
    }
    // the console reads the pixels it is about to overwrite: the draw gets a copy
    if (&t == draw_rt_) {
        if (!t.fb) {
            D3D11_TEXTURE2D_DESC td;
            t.res->GetDesc(&td);
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev_->CreateTexture2D(&td, nullptr, &t.fb)) ||
                FAILED(dev_->CreateShaderResourceView(t.fb.Get(), nullptr, &t.fb_srv)))
                return nullptr;
        }
        ctx_->CopyResource(t.fb.Get(), t.res.Get());
        return t.fb_srv.Get();
    }
    if (t.depth) DepthRead(d.format, info);
    return t.srv.Get();
}

bool D3D11Backend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi) {
    tx = Tex{};
    tx.d = d;
    tx.fmt = HostFormat(fi.host);
    uint32_t w = d.width, h = d.height;
    tx.levels = d.levels;
    if (fi.compressed && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = tx.fmt;
    if (d.depth > 1) {
        D3D11_TEXTURE3D_DESC td{w, h, d.depth, tx.levels, tx.fmt, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
        ComPtr<ID3D11Texture3D> t3;
        if (FAILED(dev_->CreateTexture3D(&td, nullptr, &t3))) return false;
        tx.res = t3;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        sv.Texture3D.MipLevels = tx.levels;
    } else {
        D3D11_TEXTURE2D_DESC td = Texture2DDesc(tx.fmt, w, h, D3D11_BIND_SHADER_RESOURCE);
        td.MipLevels = tx.levels;
        td.ArraySize = d.cube ? 6 : 1;
        td.MiscFlags = d.cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
        ComPtr<ID3D11Texture2D> t2;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &t2))) return false;
        tx.res = t2;
        sv.ViewDimension = d.cube ? D3D11_SRV_DIMENSION_TEXTURECUBE : D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = tx.levels;  // TextureCube.MipLevels at the same place
    }
    return SUCCEEDED(dev_->CreateShaderResourceView(tx.res.Get(), &sv, &tx.srv));
}

// every face and level decoded and uploaded, rows padded or cut to what the host format needs
void D3D11Backend::UploadTexture(Tex& tx, const TextureDesc& d, const uint8_t* p, const uint32_t* colours) {
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
            for (uint32_t z = 0; z < slices; z++) {
                for (uint32_t y = 0; y < n_rows && y < src_rows; y++)
                    std::memcpy(&rows[(size_t(z) * n_rows + y) * row_bytes], &px[(size_t(z) * src_rows + y) * pitch],
                                std::min(pitch, row_bytes));
            }
            ctx_->UpdateSubresource(tx.res.Get(), D3D11CalcSubresource(l, f, tx.levels), nullptr, rows.data(), row_bytes,
                                    row_bytes * n_rows);
        }
    }
}

// a P8 texture is decoded with the stage's palette: 256 colours, fewer by the size code in the top two bits
struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B;          // D3DFMT_P8
    constexpr uint32_t kPaletteCommon = 0;     // D3DPalette: Common, its top two bits the size code
    constexpr uint32_t kPaletteData = 4;       // D3DPalette: Data, physical address of the colours
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

// a texture in guest memory: made again when its layout changed, uploaded again when its bytes did
ID3D11ShaderResourceView* D3D11Backend::BindGuestTexture(const State& s, int stage, const TextureDesc& d,
                                                         const FormatInfo& fi, DrawTextures& stages) {
    if (!d.linear && !d.cube && d.depth == 1 && !fi.depth) {
        auto r = FindReplacement(d.data);
        if (ID3D11ShaderResourceView* srv = r ? BindReplacement(*r, d, stages.levels[stage]) : nullptr) {
            stages.replaced[stage] = true;
            return srv;
        }
    }
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    Tex& tx = textures_[key];
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi)) {
        textures_.erase(key);
        return nullptr;
    }
    // cheap change check once per frame: lock epochs plus a sample of the bytes
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
    return tx.srv.Get();
}

// a title's picture in place of the texture at d.data: made again when its size changed, uploaded again (with its
// levels) for each new version
ID3D11ShaderResourceView* D3D11Backend::BindReplacement(const Replacement& r, const TextureDesc& d, uint32_t& levels) {
    constexpr uint32_t kFmtA8R8G8B8 = 0x06;
    const uint64_t key = uint64_t(0xFFFFFFFFu) << 32 | d.data;  // no palette is at the top of memory
    Tex& tx = textures_[key];
    if (tx.frame != presents_ + 1 && !ReplacementHolds(r)) {
        textures_.erase(key);
        return nullptr;
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
                return nullptr;
            }
            tx.frame = presents_ + 1;
        }
        uint32_t w = r.width, h = r.height;
        for (uint32_t l = 0; l < pictures.size(); l++) {
            ctx_->UpdateSubresource(tx.res.Get(), D3D11CalcSubresource(l, 0, tx.levels), nullptr, pictures[l].data(), w * 4,
                                    w * 4 * h);
            w = std::max(w / 2, 1u);
            h = std::max(h / 2, 1u);
        }
        tx.sig = r.version;
    }
    levels = tx.levels;
    return tx.srv.Get();
}

// a cube's faces drawn on the GPU (an environment map rendered each frame) replace level 0 of the face, once a frame
void D3D11Backend::CopyCubeTargets(Tex& tx, const TextureDesc& d) {
    if (tx.fmt != kBgra || tx.faces == presents_ + 1) return;
    tx.faces = presents_ + 1;
    for (uint32_t f = 0; f < 6; f++) {
        uint32_t face = d.data + f * FaceSize(d);
        auto it = targets_.find(TargetMapKey(face, false));
        if (it == targets_.end() || TargetStale(face)) continue;
        Target& t = it->second;
        if (t.depth || t.gw != d.width || t.gh != d.height) continue;
        ComPtr<ID3D11Texture2D> src = PrepareCopySource(t);
        if (src) ctx_->CopySubresourceRegion(tx.res.Get(), D3D11CalcSubresource(0, f, tx.levels), 0, 0, 0, src.Get(), 0, nullptr);
    }
}

ID3D11ShaderResourceView* D3D11Backend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    stages.info[stage] = {};
    stages.reads.rt[stage] = false;
    stages.reads.k[stage] = 1;
    stages.reads.wide[stage] = 0;
    stages.levels[stage] = 1;
    stages.replaced[stage] = false;
    uint32_t header = s.textures[size_t(stage)];
    TextureDesc d;
    if (!header || !Describe(header, d)) return nullptr;
    const FormatInfo& fi = Format(d.format);
    stages.info[stage].kind = TextureKind(d);
    stages.info[stage].rect = d.linear;
    // rendered by the GPU: sample the target itself; DXT data loaded over a target's memory is not it
    auto tit = targets_.find(SizedTargetKey(d.data, fi.depth, d.width, d.height));
    // a depth buffer read through a 32 bit colour format (its bytes, kTexDepthBytes) is kept under its depth key
    if (tit == targets_.end() && !fi.depth && DepthBytesOrder(d.format))
        tit = targets_.find(SizedTargetKey(d.data, true, d.width, d.height));
    // a smaller depth texture on a depth buffer's memory, and no target of its size: the buffer's top left (007:
    // Everything or Nothing draws its character shadows into the corner of a 640x480 depth surface and reads them
    // through 128x128 and 256x256 headers; guest memory, which the GPU never wrote, gave a hatched patch instead)
    if (tit == targets_.end() && fi.depth && d.linear) {
        auto big = targets_.find(TargetMapKey(d.data, true));
        if (big != targets_.end() && big->second.gw >= d.width && big->second.gh >= d.height) {
            tit = big;
            stages.reads.size[stage][0] = big->second.gw;
            stages.reads.size[stage][1] = big->second.gh;
        }
    }
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed)
        return BindTargetTexture(s, stage, d, tit->second, stages);
    return BindGuestTexture(s, stage, d, fi, stages);
}

// ---- the draw

void D3D11Backend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    auto vit = shader_src_.find(sh.vkey);
    if (vit != shader_src_.end()) {
        sh.vs_code = blobs_[vit->second].Get();
        sh.vh = vit->second;
        return;
    }
    std::string src = VertexHlsl(s, a, sh);
    sh.vs_code = Compile(sh.quad ? QuadHlsl(std::move(src)) : src, "main", "vs_5_0", sh.vh);
    shader_src_[sh.vkey] = sh.vh;
}

void D3D11Backend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh) {
    // pixel shaders share the map with vertex shaders, the salt keeps their keys apart
    auto pit = shader_src_.find(sh.pkey ^ kPixelKeySalt);
    if (pit != shader_src_.end()) {
        sh.ps_code = blobs_[pit->second].Get();
        sh.ph = pit->second;
        return;
    }
    sh.ps_code = Compile(PixelHlsl(pk), "main", "ps_5_0", sh.ph);
    shader_src_[sh.pkey ^ kPixelKeySalt] = sh.ph;
}

Pipeline* D3D11Backend::EnsurePipeline(const PsoKey& k, ID3DBlob* vs) {
    uint64_t h = Hash(&k, sizeof(k));
    auto it = pipelines_.find(h);
    if (it != pipelines_.end()) return &it->second;
    Pipeline p;
    D3D11_INPUT_ELEMENT_DESC el[16];
    uint32_t n = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        el[n++] = {"ATTR", r, DXGI_FORMAT(e & 0xFF), (e >> 8) & 0xFF, e >> 16, D3D11_INPUT_PER_VERTEX_DATA, 0};
    }
    if (n && FAILED(dev_->CreateInputLayout(el, n, vs->GetBufferPointer(), vs->GetBufferSize(), &p.layout)))
        LOG_FIRST(8, "d3d11: input layout creation failed");
    D3D11_BLEND_DESC bd{};
    auto& b = bd.RenderTarget[0];
    b.BlendEnable = k.blend;
    b.SrcBlend = D3D11_BLEND(Blend(k.src, false));
    b.DestBlend = D3D11_BLEND(Blend(k.dst, false));
    b.BlendOp = D3D11_BLEND_OP(BlendOp(k.op));
    b.SrcBlendAlpha = D3D11_BLEND(Blend(k.src, true));
    b.DestBlendAlpha = D3D11_BLEND(Blend(k.dst, true));
    b.BlendOpAlpha = D3D11_BLEND_OP(BlendOp(k.op));
    b.RenderTargetWriteMask = UINT8(k.wmask);
    dev_->CreateBlendState(&bd, &p.blend);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = k.zen;
    dd.DepthWriteMask = k.zwrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_FUNC(Cmp(k.zfunc));
    dd.StencilEnable = k.sten;
    dd.StencilReadMask = UINT8(k.smask);
    dd.StencilWriteMask = UINT8(k.swmask);
    D3D11_DEPTH_STENCILOP_DESC so{D3D11_STENCIL_OP(StencilOp(k.sfail)), D3D11_STENCIL_OP(StencilOp(k.szfail)),
                                  D3D11_STENCIL_OP(StencilOp(k.spass)), D3D11_COMPARISON_FUNC(Cmp(k.sfunc))};
    dd.FrontFace = dd.BackFace = so;
    dev_->CreateDepthStencilState(&dd, &p.depth);
    // a point is drawn as its whole console pixel by a geometry shader, solid, front facing and never culled
    p.points = k.topo == uint32_t(Topology::Points);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = k.fill == kFillWireframe && !p.points ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    rd.CullMode = k.cull && !p.points ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    rd.FrontCounterClockwise = k.ccw && !p.points;
    rd.DepthBias = k.zbias;
    rd.SlopeScaledDepthBias = k.zslope;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    dev_->CreateRasterizerState(&rd, &p.raster);
    if (p.points && !point_gs_) {
        uint64_t gh;
        if (ID3DBlob* gs = Compile(PointHlsl(), "main", "gs_5_0", gh))
            dev_->CreateGeometryShader(gs->GetBufferPointer(), gs->GetBufferSize(), nullptr, &point_gs_);
    }
    return &(pipelines_[h] = std::move(p));
}

void D3D11Backend::UpdateBuffer(ID3D11Buffer* b, const void* p, uint32_t bytes) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    std::memcpy(m.pData, p, bytes);
    ctx_->Unmap(b, 0);
}

void D3D11Backend::UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                                   GuestExtent size, DrawConstants& c) {
    // each buffer written only when its contents changed
    if (vsc_last_.size() != sizeof(s.vsc) || std::memcmp(vsc_last_.data(), s.vsc.data(), sizeof(s.vsc))) {
        UpdateBuffer(vsc_cb_.Get(), s.vsc.data(), sizeof(s.vsc));
        const uint8_t* b = reinterpret_cast<const uint8_t*>(s.vsc.data());
        vsc_last_.assign(b, b + sizeof(s.vsc));
    }
    FillSurfaceConstants(s, size, c);
    // points: host pixels per console pixel
    if (k.topo == uint32_t(Topology::Points)) {
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
    if (dc_last_.size() != sizeof(c) || std::memcmp(dc_last_.data(), &c, sizeof(c))) {
        UpdateBuffer(dc_cb_.Get(), &c, sizeof(c));
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&c);
        dc_last_.assign(b, b + sizeof(c));
    }
    if (!ds_.cb) {
        ID3D11Buffer* cbs[2] = {vsc_cb_.Get(), dc_cb_.Get()};
        ctx_->VSSetConstantBuffers(0, 2, cbs);
        ctx_->PSSetConstantBuffers(0, 2, cbs);
        ctx_->GSSetConstantBuffers(0, 2, cbs);
        ds_.cb = true;
    }
}

void D3D11Backend::SetTexturesAndSamplers(const State& s, const Assembled& a, const DrawTextures& stages) {
    ctx_->PSSetShaderResources(0, 4, stages.srv);
    ID3D11SamplerState* smp[4];
    bool clamp = UnitTexcoords(a);
    uint32_t aniso = app::Get().aniso;
    for (int i = 0; i < 4; i++) {
        D3D12_SAMPLER_DESC d12 = StageSampler(s, i, stages.levels[i], stages.info[i].shadow, clamp, aniso, stages.replaced[i]);
        D3D11_SAMPLER_DESC sd;
        std::memcpy(&sd, &d12, sizeof(sd));
        if (!sd.ComparisonFunc) sd.ComparisonFunc = D3D11_COMPARISON_NEVER;  // D3D11 checks it even when unused
        sd.MaxAnisotropy = std::clamp<UINT>(sd.MaxAnisotropy, 1, 16);
        auto& state = samplers_[Hash(&sd, sizeof(sd))];
        if (!state) dev_->CreateSamplerState(&sd, &state);
        smp[i] = state.Get();
    }
    ctx_->PSSetSamplers(0, 4, smp);
}

// the viewport covers the target, the guest viewport is in the shader; the scissor clips to it
void D3D11Backend::SetViewportScissor(const State& s, uint32_t gw) {
    const Target* b = bound_rt_ ? bound_rt_ : bound_ds_;
    float W = float(b->w), H = float(b->h);
    D3D11_VIEWPORT vp{0, 0, W, H, 0, 1};
    float sx = W / gw, sy = b->gh ? H * s.aa_y / b->gh : sx;  // across and down differ for a stretched target
    LONG x0 = LONG(s.viewport.x * sx);
    LONG y0 = LONG(s.viewport.y * sy);
    LONG x1 = LONG((s.viewport.x + s.viewport.w) * sx);
    LONG y1 = LONG((s.viewport.y + s.viewport.h) * sy);
    if (s.scissor_count) {
        x0 = std::max(x0, LONG(s.scissors[0] * sx));
        y0 = std::max(y0, LONG(s.scissors[1] * sy));
        x1 = std::min(x1, LONG(s.scissors[2] * sx));
        y1 = std::min(y1, LONG(s.scissors[3] * sy));
    }
    D3D11_RECT sc{std::clamp<LONG>(x0, 0, LONG(W)), std::clamp<LONG>(y0, 0, LONG(H)), std::clamp<LONG>(x1, 0, LONG(W)),
                  std::clamp<LONG>(y1, 0, LONG(H))};
    if (!ds_.vp_ok || std::memcmp(&ds_.vp, &vp, sizeof(vp))) ctx_->RSSetViewports(1, &vp);
    if (!ds_.vp_ok || std::memcmp(&ds_.sc, &sc, sizeof(sc))) ctx_->RSSetScissorRects(1, &sc);
    ds_.vp = vp;
    ds_.sc = sc;
    ds_.vp_ok = true;
}

// where bytes go in the ring: past what is there, or at 0 after starting the ring over when they do not fit
uint32_t D3D11Backend::Reserve(Ring& r, uint32_t bytes) {
    uint32_t o = (r.used + 15) & ~15u;
    if (o + bytes > r.size) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx_->Map(r.buf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) ctx_->Unmap(r.buf.Get(), 0);
        o = 0;
    }
    r.used = o;
    return o;
}

uint32_t D3D11Backend::Push(Ring& r, uint32_t at, const void* p, uint32_t bytes) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx_->Map(r.buf.Get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m))) {
        std::memcpy(static_cast<uint8_t*>(m.pData) + at, p, bytes);
        ctx_->Unmap(r.buf.Get(), 0);
    }
    r.used = std::max(r.used, at + bytes);
    return at;
}

void D3D11Backend::DrawGeometry(const Assembled& a) {
    // every stream of the draw in one stretch of the ring; past the ring's size, buffers of their own
    uint32_t total = 0;
    for (uint32_t i = 0; i < a.buffer_count; i++) total += (a.buffers[i].bytes + 15) & ~15u;
    ID3D11Buffer* bufs[16];
    UINT strides[16], offsets[16];
    std::vector<ComPtr<ID3D11Buffer>> own;
    if (total <= vring_.size) {
        uint32_t at = Reserve(vring_, total);
        for (uint32_t i = 0; i < a.buffer_count; i++) {
            const VertexBuffer& b = a.buffers[i];
            bufs[i] = vring_.buf.Get();
            strides[i] = b.stride;
            offsets[i] = Push(vring_, at, b.src, b.bytes);
            at = (at + b.bytes + 15) & ~15u;
        }
    } else {
        for (uint32_t i = 0; i < a.buffer_count; i++) {
            const VertexBuffer& b = a.buffers[i];
            D3D11_BUFFER_DESC d{b.bytes, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER};
            D3D11_SUBRESOURCE_DATA init{b.src};
            own.emplace_back();
            dev_->CreateBuffer(&d, &init, &own.back());
            bufs[i] = own.back().Get();
            strides[i] = b.stride;
            offsets[i] = 0;
        }
    }
    if (a.buffer_count) ctx_->IASetVertexBuffers(0, a.buffer_count, bufs, strides, offsets);
    int topo = int(PrimitiveTopology(a));
    if (ds_.topo != topo) ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY(ds_.topo = topo));
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        DXGI_FORMAT fmt = a.index16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        ComPtr<ID3D11Buffer> ib;
        uint32_t at = 0;
        if (ibytes <= iring_.size) {
            at = Push(iring_, Reserve(iring_, ibytes), a.index_src, ibytes);
            ib = iring_.buf;
        } else {
            D3D11_BUFFER_DESC d{ibytes, D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER};
            D3D11_SUBRESOURCE_DATA init{a.index_src};
            dev_->CreateBuffer(&d, &init, &ib);
        }
        ctx_->IASetIndexBuffer(ib.Get(), fmt, at);
        ctx_->DrawIndexed(a.index_count, 0, a.base_vertex);
    } else {
        ctx_->Draw(a.draw_count, 0);
    }
}

void D3D11Backend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
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
    EnsureVertexShader(s, a, sh);
    // stage textures, which may draw copies, then the targets again
    DrawTextures stages;
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    for (int i = 0; i < 4; i++) stages.srv[i] = BindTexture(s, i, stages);
    if (!BindTargets(s)) return;
    // the pixel shader made for the textures
    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad;
    // D3DZB_USEW draws write w as their depth, as the console does; XBE_WBUFFER=0 goes back to z / w
    static const bool wbuffer = [] {
        const char* e = std::getenv("XBE_WBUFFER");
        return !(e && *e == '0');
    }();
    pk.wbuf = wbuffer && s.rs[kRsZEnable] == 2 && bound_ds_ ? 1 : 0;
    sh.pkey = Hash(&pk, sizeof(pk));
    EnsurePixelShader(pk, sh);
    if (!sh.vs_code || !sh.ps_code) return;

    // pipeline state
    PsoKey k;
    PipelineTargets pt{.rt = bound_rt_ != nullptr, .rtfmt = bound_rt_ ? uint32_t(kBgra) : 0u, .ds = bound_ds_ != nullptr,
                       .ds_scale = bound_ds_ ? float(bound_ds_->h) / bound_ds_->gh : 1.0f,
                       .samples = bound_ms_ ? msaa_ : 1};
    BuildPipelineKey(s, a, sh.vh, sh.ph, pt, k);
    if (!adjust.depth_test) k.zen = k.zwrite = 0;
    k.zbias += adjust.depth_bias;
    // W-buffered: the pixel shader adds the polygon offset to w (d3d12.cpp Draw)
    float wdepth[4] = {float(k.zbias) * s.zscale / kDepth24Max, k.zslope, 1.0f / (s.zscale + 1.0f),
                       sh.prog ? 1.0f : s.zscale * s.inv_wfar};
    if (pk.wbuf) k.zbias = 0, k.zslope = 0;
    Pipeline* p = EnsurePipeline(k, sh.vs_code);
    ID3D11VertexShader* vs = VertexShader(sh.vs_code, sh.vh);
    ID3D11PixelShader* ps = PixelShader(sh.ps_code, sh.ph);
    if (!vs || !ps || (k.regs && !p->layout)) return;
    ID3D11GeometryShader* gs = p->points ? point_gs_.Get() : nullptr;
    // after a pass or the overlay the context holds their state, whatever the cache says: all of it set again
    bool fresh = !ds_.set;
    ds_.set = true;
    if (fresh || ds_.vs != vs) ctx_->VSSetShader(ds_.vs = vs, nullptr, 0);
    if (fresh || ds_.ps != ps) ctx_->PSSetShader(ds_.ps = ps, nullptr, 0);
    if (fresh || ds_.gs != gs) ctx_->GSSetShader(ds_.gs = gs, nullptr, 0);
    if (fresh || ds_.layout != p->layout.Get()) ctx_->IASetInputLayout(ds_.layout = p->layout.Get());
    if (fresh || ds_.raster != p->raster.Get()) ctx_->RSSetState(ds_.raster = p->raster.Get());
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    if (fresh || ds_.blend != p->blend.Get() || std::memcmp(ds_.bf, bf, sizeof(bf))) {
        ctx_->OMSetBlendState(ds_.blend = p->blend.Get(), bf, 0xFFFFFFFF);
        std::memcpy(ds_.bf, bf, sizeof(bf));
    }
    uint32_t sref = SimpleState(s, kMStencilRef) & 0xFF;
    if (fresh || ds_.depth != p->depth.Get() || ds_.sref != sref)
        ctx_->OMSetDepthStencilState(ds_.depth = p->depth.Get(), ds_.sref = sref);

    // constants
    GuestExtent size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    size = DrawnExtent(s, size);
    DrawConstants c{};
    std::memcpy(c.wdepth, wdepth, sizeof(wdepth));
    float stretch = bound_rt_ ? XStretch(aspect_, guest_h_, bound_rt_->gw, bound_rt_->gh) : 0;
    c.hud[0] = HudSqueeze(s, a, aspect_, stretch);
    c.hud[2] = DepthTested(s) ? 1.0f : 0.0f;
    if (bound_rt_)
        NoteSqueeze(s, a, bound_rt_->data, bound_rt_->gw, bound_rt_->gh, bound_rt_->w, aspect_, stretch, c.hud[0]);
    c.hud[1] = (bound_rt_ ? bound_rt_->gw : guest_w_) * 0.5f / s.aa_x;
    UploadConstants(s, sh, k, stages, size, c);
    SetTexturesAndSamplers(s, a, stages);
    SetViewportScissor(s, size.w);

    // geometry, counted while the title runs a visibility test
    ComPtr<ID3D11Query> query;
    if (s.simple[kMCountEnable >> 2] && (query = NewQuery())) ctx_->Begin(query.Get());
    DrawGeometry(a);
    if (query) {
        ctx_->End(query.Get());
        counted_.push_back({query, SamplesPerPixel()});
    }
}

// ---- visibility tests

ComPtr<ID3D11Query> D3D11Backend::NewQuery() {
    if (!free_queries_.empty()) {
        ComPtr<ID3D11Query> q = std::move(free_queries_.back());
        free_queries_.pop_back();
        return q;
    }
    D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION, 0};
    ComPtr<ID3D11Query> q;
    if (FAILED(dev_->CreateQuery(&qd, &q))) LOG_FIRST(1, "d3d11: no occlusion queries, visibility tests stay pending");
    return q;
}

// the console counts its own pixels: the bound target's scale comes out
float D3D11Backend::SamplesPerPixel() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw || !t->gh) return 1;
    return float(t->w) * float(t->h) / (float(t->gw) * float(t->gh)) * float(bound_ms_ ? msaa_ : 1);
}

// reports whose draws the GPU has finished, written as the console's GPU writes them: time, count, then 0 for done
void D3D11Backend::DeliverReports(bool wait) {
    constexpr uint32_t kTime = 0, kTimeHigh = 4, kCount = 8, kStatus = 12;
    while (!reports_.empty()) {
        Report& r = reports_.front();
        double pixels = 0;
        for (const Query& q : r.queries) {
            UINT64 n = 0;
            HRESULT hr;
            while ((hr = ctx_->GetData(q.q.Get(), &n, sizeof(n), wait ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE && wait)
                std::this_thread::yield();
            if (hr != S_OK) return;  // not finished: this and every later report wait
            pixels += double(n) / q.samples_per_pixel;
        }
        kernel::W32(r.va + kTime, uint32_t(presents_));
        kernel::W32(r.va + kTimeHigh, 0);
        kernel::W32(r.va + kCount, uint32_t(std::lround(pixels)));
        std::atomic_thread_fence(std::memory_order_release);
        kernel::W32(r.va + kStatus, 0);
        // its queries are free again once no later report and no count in progress holds them
        for (const Query& q : r.queries) {
            bool held = false;
            for (size_t i = 1; i < reports_.size() && !held; i++)
                for (const Query& o : reports_[i].queries) held |= o.q == q.q;
            for (const Query& c : counted_) held |= c.q == q.q;
            if (!held && std::find(free_queries_.begin(), free_queries_.end(), q.q) == free_queries_.end())
                free_queries_.push_back(q.q);
        }
        reports_.pop_front();
    }
}

// ---- present

void D3D11Backend::UploadGammaRamp() {
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
    ctx_->UpdateSubresource(lut_.Get(), 0, nullptr, px, sizeof(px), 0);
    gamma_ = !identity;
}

void D3D11Backend::ResizeSwap() {
    RECT rc;
    GetClientRect(static_cast<HWND>(app::Window()), &rc);
    uint32_t w = std::max<LONG>(rc.right - rc.left, 1);
    uint32_t h = std::max<LONG>(rc.bottom - rc.top, 1);
    if (w == bb_w_ && h == bb_h_) return;
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    Unbind();
    bb_rtv_.Reset();
    swap_->ResizeBuffers(kBuffers, w, h, kBgra, tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    CreateBackBufferView();
    bb_w_ = w;
    bb_h_ = h;
}

// the frame at its shape (16:9 or 4:3, ultrawide the screen's), centred on black
void D3D11Backend::BlitFrame(Target& t) {
    float aspect = app::FrameAspect();
    float w = float(bb_w_), h = float(bb_h_);
    float vw = std::min(w, h * aspect);
    float vh = vw / aspect;
    D3D11_VIEWPORT vp{(w - vw) / 2, (h - vh) / 2, vw, vh, 0, 1};
    D3D11_RECT sc{0, 0, LONG(bb_w_), LONG(bb_h_)};
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetScissorRects(1, &sc);
    float uv[4] = {0, 0, 1, 1};
    Blit(t.srv.Get(), uv, {.gamma = gamma_, .fxaa = app::Get().antialiasing == 1});
}

void D3D11Backend::RenderUi() {
    std::lock_guard lock(app::UiLock());
    app::FeedPad();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    app::DrawHud(fps_);
    app::DrawOverlay();
    if (app::kGame.ui) app::kGame.ui(fps_);
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    ForgetDrawState();
}

void D3D11Backend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;    // presents between sweeps
    constexpr size_t kTexturesKept = 2048;  // no sweep below this many
    constexpr uint64_t kIdleFrames = 600;
    // textures unused for ten seconds go, levels stream theirs in and out
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) it = textures_.erase(it);
        else ++it;
    }
}

void D3D11Backend::CountFps() {
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_ = fps_n_ / el;
        fps_n_ = 0;
        fps_t_ = now;
    }
}

void D3D11Backend::Present(const Surface& front) {
    ResizeSwap();
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (t) Settle(*t);
    if (ramp_dirty_) UploadGammaRamp();
    float black[4] = {0, 0, 0, 1};
    ctx_->ClearRenderTargetView(bb_rtv_.Get(), black);
    SetPassTarget(bb_rtv_.Get(), nullptr, bb_w_, bb_h_);
    if (t) BlitFrame(*t);
    D3D11_VIEWPORT vp{0, 0, float(bb_w_), float(bb_h_), 0, 1};
    ctx_->RSSetViewports(1, &vp);
    RenderUi();
    bool vsync = app::Get().vsync;
    swap_->Present(vsync ? 1 : 0, !vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    // the flip model unbinds the back buffer
    Unbind();
    DeliverReports(false);
    presents_++;
    presents_seen_.store(presents_);
    ReleaseOldTextures();
    CountFps();
    ApplySettings();
}

void D3D11Backend::ApplySettings() {
    // antialiasing 2, 3, 4: MSAA 2x, 4x, 8x; a change settles and drops every twin, the next draws make new ones
    static const uint32_t kSamples[] = {1, 1, 2, 4, 8};
    uint32_t want_ms = kSamples[std::min<uint32_t>(app::Get().antialiasing, 4)];
    if (want_ms != msaa_) {
        for (auto& [key, t] : targets_) DropMs(t);
        msaa_ = want_ms;
        Unbind();
        LOGR("d3d11: MSAA %ux", msaa_);
    }
    float aspect = app::FrameAspect();
    if (std::fabs(aspect - aspect_) > 1e-4f) {
        aspect_ = aspect;
        RescaleTargets();
    }
    // a new scale waits until it has held for half a second: dragging the slider would remake every target each frame
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

// a new render scale or shape: colour targets keep their pictures, scaled into new ones (a title may draw a target
// once and sample it for long after: PGR2's mini map), depth targets start again
void D3D11Backend::RescaleTargets() {
    for (auto& [key, t] : targets_) DropMs(t);
    Unbind();
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
        SetPassTarget(t.rtv.Get(), nullptr, t.w, t.h);
        float uv[4] = {0, 0, 1, 1};
        Blit(old.srv.Get(), uv, {.alpha = true});
        t.data = old.data;
        t.bytes = old.bytes;
        t.epoch = old.epoch;
        t.sig = old.sig;
        t.drawn = old.drawn;
        t.linear32 = old.linear32;
        t.read = std::move(old.read);
        old = std::move(t);
        ++it;
    }
    std::lock_guard lock(late_lock_);
    late_.clear();
    LOGR("d3d11: render scale %.2f, %zu targets kept", scale_, targets_.size());
}

}  // namespace

std::unique_ptr<Backend> CreateD3D11Backend() { return std::make_unique<D3D11Backend>(); }

}  // namespace gpu
