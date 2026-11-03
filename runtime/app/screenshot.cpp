// F12: the frame as shown, without the settings window, as a PNG in the screenshots folder beside the exe. The renderer
// reads its back buffer before it draws the interface; the encoding runs on a thread of its own, so a frame loses
// only the read.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

#include "app/app.h"
#include "core/log.h"

using Microsoft::WRL::ComPtr;

namespace app {

namespace {

std::atomic<bool> g_requested{false};
std::atomic<int64_t> g_saved_ms{0};  // when the last one was written

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool WritePng(const std::filesystem::path& path, const std::vector<uint8_t>& bgra, uint32_t w, uint32_t h) {
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(w, h);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr) && format != GUID_WICPixelFormat32bppBGRA) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = frame->WritePixels(h, w * 4, UINT(bgra.size()), const_cast<BYTE*>(bgra.data()));
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    if (FAILED(hr)) LOGR("screenshot: could not write %s (%08X)", path.filename().u8string().c_str(), unsigned(hr));
    return SUCCEEDED(hr);
}

}  // namespace

void RequestScreenshot() { g_requested = true; }
bool TakeScreenshotRequest() { return g_requested.exchange(false); }
bool ScreenshotJustSaved() { return NowMs() - g_saved_ms.load() < 3000 && g_saved_ms.load(); }

void SaveScreenshot(std::vector<uint8_t> bgra, uint32_t w, uint32_t h) {
    for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;  // a back buffer's alpha means nothing
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf(name, 64, L"%ls %04u-%02u-%02u %02u-%02u-%02u.png", kGame.short_title, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::filesystem::path dir = ExeDir() / "screenshots";
    std::thread([bgra = std::move(bgra), w, h, dir, path = dir / name] {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (WritePng(path, bgra, w, h)) {
            g_saved_ms = NowMs();
            LOGR("screenshot: %s (%ux%u)", path.filename().u8string().c_str(), w, h);
        }
        CoUninitialize();
    }).detach();
}

}  // namespace app
