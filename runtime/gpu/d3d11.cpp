// Direct3D 11 renderer (renderer = "d3d11" in the settings file), for GPUs and drivers where Direct3D 12 runs badly.
// In progress: the device, the swap chain, presenting and the settings window work; the title's draws are not
// drawn yet, copies and visibility tests are answered from guest memory as the headless renderer does.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <mutex>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/texture.h"
#include "kernel/kernel.h"

using Microsoft::WRL::ComPtr;

namespace gpu {

namespace {

constexpr UINT kBuffers = 2;
constexpr DXGI_FORMAT kBackFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

class D3D11Backend final : public Backend {
public:
    ~D3D11Backend() override {
        if (ui_) ImGui_ImplDX11_Shutdown();
    }

    bool Init(uint32_t width, uint32_t height) override {
        guest_w_ = width;
        guest_h_ = height;
        HWND hwnd = static_cast<HWND>(app::Window());
        if (!CreateDevice() || !CreateSwapChain(hwnd)) return false;
        ui_ = ImGui_ImplDX11_Init(dev_.Get(), ctx_.Get());
        LOGI("d3d11: %ux%u guest, %ux%u window", width, height, bb_w_, bb_h_);
        return true;
    }

    void Clear(const State&, uint32_t, const uint8_t*, uint32_t, uint32_t, float, uint32_t) override {}
    void Draw(const State&, uint32_t, uint32_t, uint32_t, const uint8_t*) override {}
    void Copy(const Surface& src, const Surface& dst, uint32_t n, uint32_t rects, uint32_t points) override {
        CopySurfaceMemory(src, dst, n, rects, points);
    }

    void Present(const Surface&) override {
        ResizeSwap();
        float black[4] = {0, 0, 0, 1};
        ctx_->ClearRenderTargetView(bb_rtv_.Get(), black);
        ID3D11RenderTargetView* rtv = bb_rtv_.Get();
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{0, 0, float(bb_w_), float(bb_h_), 0, 1};
        ctx_->RSSetViewports(1, &vp);
        RenderUi();
        bool vsync = app::Get().vsync;
        swap_->Present(vsync ? 1 : 0, !vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
        CountFps();
    }

    void Sync(uint32_t, uint32_t) override {}
    void SyncLate(uint32_t) override {}
    void Flush() override {}
    void SetGamma(const uint8_t*) override {}
    // nothing is drawn yet, so a visibility test is answered at once with no pixels (as the headless renderer does)
    void GetReport(uint32_t report) override {
        constexpr uint32_t kTime = 0, kTimeHigh = 4, kCount = 8, kStatus = 12;
        kernel::W32(report + kTime, uint32_t(presents_));
        kernel::W32(report + kTimeHigh, 0);
        kernel::W32(report + kCount, 0);
        kernel::W32(report + kStatus, 0);
    }

private:
    bool CreateDevice() {
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
                                               levels, UINT(std::size(levels)), D3D11_SDK_VERSION, &dev_, nullptr,
                                               &ctx_));
        };
        if (!create(adapter.Get()) && !create((adapter = nullptr).Get())) {
            LOGE("d3d11: no device");
            return false;
        }
        DXGI_ADAPTER_DESC1 ad{};
        if (adapter) adapter->GetDesc1(&ad);
        LOGR("D3D11 adapter: %ls, feature level %X", adapter ? ad.Description : L"default",
             unsigned(dev_->GetFeatureLevel()));
        ComPtr<IDXGIFactory5> f5;
        BOOL tear = FALSE;
        if (SUCCEEDED(factory_.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof(tear));
        tearing_ = tear;
        return true;
    }

    bool CreateSwapChain(HWND hwnd) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        bb_w_ = std::max<LONG>(rc.right - rc.left, 1);
        bb_h_ = std::max<LONG>(rc.bottom - rc.top, 1);
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = bb_w_;
        sd.Height = bb_h_;
        sd.Format = kBackFormat;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kBuffers;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(factory_->CreateSwapChainForHwnd(dev_.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) {
            LOGE("d3d11: no swap chain");
            return false;
        }
        factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        swap_ = sc1;
        CreateBackBufferView();
        return true;
    }

    void CreateBackBufferView() {
        ComPtr<ID3D11Texture2D> bb;
        swap_->GetBuffer(0, IID_PPV_ARGS(&bb));
        dev_->CreateRenderTargetView(bb.Get(), nullptr, &bb_rtv_);
    }

    void ResizeSwap() {
        RECT rc;
        GetClientRect(static_cast<HWND>(app::Window()), &rc);
        uint32_t w = std::max<LONG>(rc.right - rc.left, 1);
        uint32_t h = std::max<LONG>(rc.bottom - rc.top, 1);
        if (w == bb_w_ && h == bb_h_) return;
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        bb_rtv_.Reset();
        swap_->ResizeBuffers(kBuffers, w, h, kBackFormat, tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        CreateBackBufferView();
        bb_w_ = w;
        bb_h_ = h;
    }

    void RenderUi() {
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
    }

    void CountFps() {
        presents_++;
        fps_n_++;
        auto now = std::chrono::steady_clock::now();
        double el = std::chrono::duration<double>(now - fps_t_).count();
        if (el >= 0.5) {
            fps_ = fps_n_ / el;
            fps_n_ = 0;
            fps_t_ = now;
        }
    }

    ComPtr<IDXGIFactory2> factory_;
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<ID3D11RenderTargetView> bb_rtv_;
    uint32_t guest_w_ = 0, guest_h_ = 0;
    uint32_t bb_w_ = 0, bb_h_ = 0;
    bool tearing_ = false;
    bool ui_ = false;
    uint64_t presents_ = 0;
    double fps_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

}  // namespace

std::unique_ptr<Backend> CreateD3D11Backend() { return std::make_unique<D3D11Backend>(); }

}  // namespace gpu
