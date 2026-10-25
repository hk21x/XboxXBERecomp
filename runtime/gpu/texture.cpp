#include "gpu/texture.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "app/app.h"
#include "core/log.h"
#include "gpu/shaders.h"
#include "kernel/kernel.h"

namespace gpu {

namespace {

const FormatInfo kUnknown{"unknown", 32, false, false, false, Host::None};

struct Entry {
    uint32_t id;
    FormatInfo info;
};

// console texture formats (XDK D3DFORMAT values) and how they reach the host
const Entry kFormats[] = {
    {0x00, {"L8", 8, true, false, false, Host::BGRA8}},
    {0x01, {"AL8", 8, true, false, false, Host::BGRA8}},
    {0x02, {"A1R5G5B5", 16, true, false, false, Host::BGRA8}},
    {0x03, {"X1R5G5B5", 16, true, false, false, Host::BGRA8}},
    {0x04, {"A4R4G4B4", 16, true, false, false, Host::BGRA8}},
    {0x05, {"R5G6B5", 16, true, false, false, Host::BGRA8}},
    {0x06, {"A8R8G8B8", 32, true, false, false, Host::BGRA8}},
    {0x07, {"X8R8G8B8", 32, true, false, false, Host::BGRA8}},
    {0x0B, {"P8", 8, true, false, false, Host::BGRA8}},
    {0x0C, {"DXT1", 8, false, true, false, Host::BC1}},
    {0x0E, {"DXT3", 16, false, true, false, Host::BC2}},
    {0x0F, {"DXT5", 16, false, true, false, Host::BC3}},
    {0x10, {"LIN_A1R5G5B5", 16, false, false, false, Host::BGRA8}},
    {0x11, {"LIN_R5G6B5", 16, false, false, false, Host::BGRA8}},
    {0x12, {"LIN_A8R8G8B8", 32, false, false, false, Host::BGRA8}},
    {0x13, {"LIN_L8", 8, false, false, false, Host::BGRA8}},
    {0x16, {"LIN_R8B8", 16, false, false, false, Host::BGRA8}},
    {0x17, {"LIN_G8B8", 16, false, false, false, Host::BGRA8}},
    {0x19, {"A8", 8, true, false, false, Host::BGRA8}},
    {0x1A, {"A8L8", 16, true, false, false, Host::BGRA8}},
    {0x1B, {"LIN_AL8", 8, false, false, false, Host::BGRA8}},
    {0x1C, {"LIN_X1R5G5B5", 16, false, false, false, Host::BGRA8}},
    {0x1D, {"LIN_A4R4G4B4", 16, false, false, false, Host::BGRA8}},
    {0x1E, {"LIN_X8R8G8B8", 32, false, false, false, Host::BGRA8}},
    {0x1F, {"LIN_A8", 8, false, false, false, Host::BGRA8}},
    {0x20, {"LIN_A8L8", 16, false, false, false, Host::BGRA8}},
    {0x24, {"YUY2", 16, false, false, false, Host::BGRA8}},
    {0x25, {"UYVY", 16, false, false, false, Host::BGRA8}},
    {0x27, {"L6V5U5", 16, true, false, false, Host::BGRA8}},
    {0x28, {"V8U8", 16, true, false, false, Host::BGRA8}},
    {0x29, {"R8B8", 16, true, false, false, Host::BGRA8}},
    {0x2A, {"D24S8", 32, true, false, true, Host::R32F}},
    {0x2B, {"F24S8", 32, true, false, true, Host::R32F}},
    {0x2C, {"D16", 16, true, false, true, Host::R32F}},
    {0x2D, {"F16", 16, true, false, true, Host::R32F}},
    {0x2E, {"LIN_D24S8", 32, false, false, true, Host::R32F}},
    {0x2F, {"LIN_F24S8", 32, false, false, true, Host::R32F}},
    {0x30, {"LIN_D16", 16, false, false, true, Host::R32F}},
    {0x31, {"LIN_F16", 16, false, false, true, Host::R32F}},
    {0x32, {"L16", 16, true, false, false, Host::BGRA8}},
    {0x33, {"V16U16", 32, true, false, false, Host::BGRA8}},
    {0x35, {"LIN_L16", 16, false, false, false, Host::BGRA8}},
    {0x36, {"LIN_V16U16", 32, false, false, false, Host::BGRA8}},
    {0x37, {"LIN_L6V5U5", 16, false, false, false, Host::BGRA8}},
    {0x38, {"R5G5B5A1", 16, true, false, false, Host::BGRA8}},
    {0x39, {"R4G4B4A4", 16, true, false, false, Host::BGRA8}},
    {0x3A, {"A8B8G8R8", 32, true, false, false, Host::BGRA8}},
    {0x3B, {"B8G8R8A8", 32, true, false, false, Host::BGRA8}},
    {0x3C, {"R8G8B8A8", 32, true, false, false, Host::BGRA8}},
    {0x3D, {"LIN_R5G5B5A1", 16, false, false, false, Host::BGRA8}},
    {0x3E, {"LIN_R4G4B4A4", 16, false, false, false, Host::BGRA8}},
    {0x3F, {"LIN_A8B8G8R8", 32, false, false, false, Host::BGRA8}},
    {0x40, {"LIN_B8G8R8A8", 32, false, false, false, Host::BGRA8}},
    {0x41, {"LIN_R8G8B8A8", 32, false, false, false, Host::BGRA8}},
};

uint32_t Log2(uint32_t v) {
    uint32_t n = 0;
    while ((1u << n) < v) n++;
    return n;
}

// Morton order: bits of x, y and z interleave from the lowest, an axis drops out once its size is used up
void SwizzleMasks(uint32_t w, uint32_t h, uint32_t d, uint32_t& mx, uint32_t& my, uint32_t& mz) {
    mx = my = mz = 0;
    uint32_t bit = 1, lx = Log2(w), ly = Log2(h), lz = Log2(d);
    for (uint32_t i = 0; bit && (i < lx || i < ly || i < lz); i++) {
        if (i < lx) {
            mx |= bit;
            bit <<= 1;
        }
        if (i < ly) {
            my |= bit;
            bit <<= 1;
        }
        if (i < lz) {
            mz |= bit;
            bit <<= 1;
        }
    }
}

uint32_t Deposit(uint32_t v, uint32_t mask) {
    uint32_t r = 0;
    for (uint32_t bit = 1; mask; bit <<= 1) {
        uint32_t low = mask & (~mask + 1);
        if (v & bit) r |= low;
        mask &= mask - 1;
    }
    return r;
}

uint8_t Expand5(uint32_t v) { return uint8_t((v << 3) | (v >> 2)); }
uint8_t Expand6(uint32_t v) { return uint8_t((v << 2) | (v >> 4)); }
uint8_t Expand4(uint32_t v) { return uint8_t(v * 17); }
uint8_t Clamp255(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

// one texel to B, G, R, A
void Texel(uint32_t fmt, const uint8_t* p, uint8_t* o) {
    uint16_t s16;
    std::memcpy(&s16, p, 2);
    switch (fmt) {
        case 0x00: case 0x13:
            o[0] = o[1] = o[2] = p[0];
            o[3] = 255;
            break;
        case 0x01: case 0x1B:
            o[0] = o[1] = o[2] = o[3] = p[0];
            break;
        case 0x19: case 0x1F:
            o[0] = o[1] = o[2] = 255;
            o[3] = p[0];
            break;
        case 0x1A: case 0x20:
            o[0] = o[1] = o[2] = p[0];
            o[3] = p[1];
            break;
        case 0x02: case 0x10:
            o[0] = Expand5(s16 & 31);
            o[1] = Expand5((s16 >> 5) & 31);
            o[2] = Expand5((s16 >> 10) & 31);
            o[3] = (s16 & 0x8000) ? 255 : 0;
            break;
        case 0x03: case 0x1C:
            o[0] = Expand5(s16 & 31);
            o[1] = Expand5((s16 >> 5) & 31);
            o[2] = Expand5((s16 >> 10) & 31);
            o[3] = 255;
            break;
        case 0x04: case 0x1D:
            o[0] = Expand4(s16 & 15);
            o[1] = Expand4((s16 >> 4) & 15);
            o[2] = Expand4((s16 >> 8) & 15);
            o[3] = Expand4(s16 >> 12);
            break;
        case 0x05: case 0x11:
            o[0] = Expand5(s16 & 31);
            o[1] = Expand6((s16 >> 5) & 63);
            o[2] = Expand5(s16 >> 11);
            o[3] = 255;
            break;
        case 0x06: case 0x12:
            std::memcpy(o, p, 4);
            break;
        case 0x07: case 0x1E:
            std::memcpy(o, p, 3);
            o[3] = 255;
            break;
        case 0x3A: case 0x3F:  // memory R G B A
            o[0] = p[2];
            o[1] = p[1];
            o[2] = p[0];
            o[3] = p[3];
            break;
        case 0x3B: case 0x40:  // memory A R G B
            o[0] = p[3];
            o[1] = p[2];
            o[2] = p[1];
            o[3] = p[0];
            break;
        case 0x3C: case 0x41:  // memory A B G R
            o[0] = p[1];
            o[1] = p[2];
            o[2] = p[3];
            o[3] = p[0];
            break;
        case 0x38: case 0x3D:
            o[3] = (s16 & 1) ? 255 : 0;
            o[0] = Expand5((s16 >> 1) & 31);
            o[1] = Expand5((s16 >> 6) & 31);
            o[2] = Expand5(s16 >> 11);
            break;
        case 0x39: case 0x3E:
            o[3] = Expand4(s16 & 15);
            o[0] = Expand4((s16 >> 4) & 15);
            o[1] = Expand4((s16 >> 8) & 15);
            o[2] = Expand4(s16 >> 12);
            break;
        case 0x28: case 0x17:  // two channels, low byte then high byte
            o[2] = p[0];
            o[1] = p[1];
            o[0] = p[0];
            o[3] = p[1];
            break;
        case 0x29: case 0x16:
            o[2] = p[1];
            o[1] = p[0];
            o[0] = p[0];
            o[3] = p[1];
            break;
        case 0x27: case 0x37:
            o[2] = Expand5(s16 & 31);
            o[1] = Expand5((s16 >> 5) & 31);
            o[0] = Expand6(s16 >> 10);
            o[3] = 255;
            break;
        case 0x32: case 0x35:
            o[0] = o[1] = o[2] = p[1];
            o[3] = 255;
            break;
        case 0x33: case 0x36:
            o[2] = p[1];
            o[1] = p[3];
            o[0] = 0;
            o[3] = 255;
            break;
        default:
            o[0] = o[1] = o[2] = p[0];
            o[3] = 255;
            break;
    }
}

float DepthTexel(uint32_t fmt, const uint8_t* p) {
    uint32_t v;
    uint16_t s;
    std::memcpy(&v, p, 4);
    std::memcpy(&s, p, 2);
    switch (fmt) {
        case 0x2A: case 0x2E: return float(v >> 8) / 16777215.0f;
        case 0x2C: case 0x30: return float(s) / 65535.0f;
        case 0x2B: case 0x2F: {  // unsigned, 8 bit exponent, 16 bit mantissa: a float32 missing its low bits
            uint32_t exponent = v >> 24, mantissa = (v >> 8) & 0xFFFF;
            return std::bit_cast<float>(exponent << 23 | mantissa << 7);
        }
        default: {  // unsigned, 4 bit exponent biased 7, 12 bit mantissa, over its max 511.9375
            uint32_t exponent = (s >> 12) + 120u, mantissa = s & 0xFFFu;
            return std::bit_cast<float>(exponent << 23 | mantissa << 11) / 511.9375f;
        }
    }
}

std::mutex g_dirty_lock;
std::unordered_map<uint32_t, uint64_t> g_dirty;
uint64_t g_epoch = 1;

}  // namespace

const FormatInfo& Format(uint32_t f) {
    for (auto& e : kFormats)
        if (e.id == f) return e.info;
    return kUnknown;
}

bool Describe(uint32_t header, TextureDesc& d) {
    using namespace kernel;
    if (!header) return false;
    uint32_t common = R32(header);
    uint32_t type = common & 0x70000;  // 0x40000 texture, 0x50000 surface
    if (type != 0x40000 && type != 0x50000) return false;
    // D3DPixelContainer: Common, Data, Lock, Format, Size
    uint32_t fmt = R32(header + 12), size = R32(header + 16);
    d.data = mem::VirtualFromPhysical(R32(header + 4));
    // Format: cube bit 2, dimensions bits 4 to 7, format 8 to 15, levels 16 to 19, log2 sizes from bit 20
    d.format = (fmt >> 8) & 0xFF;
    d.cube = fmt & 4;
    uint32_t dims = (fmt >> 4) & 0xF;
    d.levels = std::max<uint32_t>((fmt >> 16) & 0xF, 1);
    if (size) {
        d.width = (size & 0xFFF) + 1;
        d.height = ((size >> 12) & 0xFFF) + 1;
        d.pitch = ((size >> 24) + 1) * 64;
        d.depth = 1;
        d.levels = 1;
        d.linear = true;
    } else {
        d.width = 1u << ((fmt >> 20) & 0xF);
        d.height = 1u << ((fmt >> 24) & 0xF);
        d.depth = dims == 3 ? 1u << ((fmt >> 28) & 0xF) : 1;
        d.linear = !Format(d.format).swizzled && !Format(d.format).compressed;
        d.pitch = d.width * Format(d.format).bits / 8;
    }
    return true;
}

bool DescribeSurface(const Surface& s, TextureDesc& d) {
    if (!s.header) return false;
    return Describe(s.header, d);
}

uint32_t LevelSize(const TextureDesc& d, uint32_t level) {
    const FormatInfo& f = Format(d.format);
    if (d.linear) return d.pitch * d.height;
    uint32_t w = std::max(d.width >> level, 1u), h = std::max(d.height >> level, 1u), z = std::max(d.depth >> level, 1u);
    if (f.compressed) return std::max(1u, (w + 3) / 4) * std::max(1u, (h + 3) / 4) * f.bits * z;
    return w * h * z * f.bits / 8;
}

uint32_t LevelOffset(const TextureDesc& d, uint32_t level) {
    uint32_t off = 0;
    for (uint32_t l = 0; l < level; l++) off += LevelSize(d, l);
    return off;
}

uint32_t FaceSize(const TextureDesc& d) { return (LevelOffset(d, d.levels) + 127) & ~127u; }

uint32_t TotalSize(const TextureDesc& d) { return d.cube ? FaceSize(d) * 6 : LevelOffset(d, d.levels); }

void DecodeLevel(const TextureDesc& d, uint32_t level, uint32_t face, const uint8_t* guest, const uint32_t* palette,
                 std::vector<uint8_t>& out, uint32_t& row_pitch) {
    const FormatInfo& f = Format(d.format);
    uint32_t w = d.linear ? d.width : std::max(d.width >> level, 1u);
    uint32_t h = d.linear ? d.height : std::max(d.height >> level, 1u);
    uint32_t z = d.linear ? 1 : std::max(d.depth >> level, 1u);
    const uint8_t* src = guest + (d.cube ? face * FaceSize(d) : 0) + LevelOffset(d, level);
    if (f.compressed) {
        uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
        row_pitch = bw * f.bits;
        out.assign(src, src + size_t(row_pitch) * bh * z);
        return;
    }
    if (f.host == Host::R32F) {
        row_pitch = w * 4;
        out.resize(size_t(row_pitch) * h * z);
        float* o = reinterpret_cast<float*>(out.data());
        uint32_t bpp = f.bits / 8;
        uint32_t mx, my, mz;
        SwizzleMasks(w, h, z, mx, my, mz);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t off = d.linear ? y * d.pitch + x * bpp : (Deposit(x, mx) | Deposit(y, my)) * bpp;
                o[y * w + x] = DepthTexel(d.format, src + off);
            }
        return;
    }
    row_pitch = w * 4;
    out.resize(size_t(row_pitch) * h * z);
    uint32_t bpp = f.bits / 8;
    if (d.format == 0x24 || d.format == 0x25) {
        // packed 4:2:2, two pixels share chroma
        bool yuy2 = d.format == 0x24;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x + 1 < w + 1; x += 2) {
                const uint8_t* p = src + y * d.pitch + x * 2;
                int y0 = yuy2 ? p[0] : p[1], u = yuy2 ? p[1] : p[0], y1 = yuy2 ? p[2] : p[3], v = yuy2 ? p[3] : p[2];
                for (int k = 0; k < 2 && x + uint32_t(k) < w; k++) {
                    int c = (k ? y1 : y0) - 16, dd = u - 128, e = v - 128;  // BT.601 studio range
                    uint8_t* o = &out[(y * w + x + uint32_t(k)) * 4];
                    o[2] = Clamp255((298 * c + 409 * e + 128) >> 8);
                    o[1] = Clamp255((298 * c - 100 * dd - 208 * e + 128) >> 8);
                    o[0] = Clamp255((298 * c + 516 * dd + 128) >> 8);
                    o[3] = 255;
                }
            }
        return;
    }
    if (d.linear) {
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) Texel(d.format, src + y * d.pitch + x * bpp, &out[(y * w + x) * 4]);
        return;
    }
    uint32_t mx, my, mz;
    SwizzleMasks(w, h, z, mx, my, mz);
    std::vector<uint32_t> xo(w), yo(h), zo(z);
    for (uint32_t x = 0; x < w; x++) xo[x] = Deposit(x, mx);
    for (uint32_t y = 0; y < h; y++) yo[y] = Deposit(y, my);
    for (uint32_t k = 0; k < z; k++) zo[k] = Deposit(k, mz);
    for (uint32_t k = 0; k < z; k++)
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t* p = src + size_t(xo[x] | yo[y] | zo[k]) * bpp;
                uint8_t* o = &out[((size_t(k) * h + y) * w + x) * 4];
                if (d.format == 0x0B && palette) {
                    uint32_t c = palette[p[0]];
                    std::memcpy(o, &c, 4);
                } else {
                    Texel(d.format, p, o);
                }
            }
}

uint32_t LinearColorFormat(uint32_t f) {
    switch (f) {
        case 0x06: return 0x12;
        case 0x07: return 0x1E;
        case 0x05: return 0x11;
        case 0x02: return 0x10;
        case 0x03: return 0x1C;
        default: return Format(f).swizzled ? 0x1E : (f ? f : 0x1E);
    }
}

uint32_t LinearDepthFormat(uint32_t f) {
    switch (f) {
        case 0x2A: return 0x2E;
        case 0x2B: return 0x2F;
        case 0x2C: return 0x30;
        case 0x2D: return 0x31;
        default: return Format(f).depth ? f : 0x2E;
    }
}

void SurfaceSize(const Surface& s, uint32_t& w, uint32_t& h) {
    TextureDesc d;
    if (Describe(s.header, d)) {
        w = d.width;
        h = d.height;
    } else {
        w = 640;
        h = 480;
    }
}

void MarkDirty(uint32_t res) {
    if (!res) return;
    uint32_t data = mem::VirtualFromPhysical(kernel::R32(res + 4));
    std::lock_guard<std::mutex> lock(g_dirty_lock);
    g_dirty[data] = ++g_epoch;
}

uint64_t DirtyEpoch(uint32_t data) {
    std::lock_guard<std::mutex> lock(g_dirty_lock);
    auto it = g_dirty.find(data);
    return it == g_dirty.end() ? 0 : it->second;
}

namespace {
std::mutex g_replace_lock;
std::unordered_map<uint32_t, std::shared_ptr<const Replacement>> g_replace;
std::atomic<size_t> g_replace_count{0};  // every bind looks, so none is checked without the lock
uint64_t g_replace_version = 0;
}  // namespace

static void RegisterReplacement(std::shared_ptr<Replacement> r) {
    std::lock_guard<std::mutex> lock(g_replace_lock);
    r->version = ++g_replace_version;
    g_replace[r->data] = std::move(r);
    g_replace_count = g_replace.size();
}

void SetReplacement(uint32_t data, uint32_t guest_bytes, uint32_t width, uint32_t height, std::vector<uint32_t> bgra) {
    if (!width || !height || bgra.size() != size_t(width) * height || !guest_bytes) return;
    auto r = std::make_shared<Replacement>();
    r->data = data;
    r->guest_bytes = guest_bytes;
    r->guest_hash = Hash(kernel::HostPtr(data), guest_bytes);
    r->width = width;
    r->height = height;
    r->bgra = std::move(bgra);
    RegisterReplacement(std::move(r));
}

void ClearReplacement(uint32_t data) {
    std::lock_guard<std::mutex> lock(g_replace_lock);
    g_replace.erase(data);
    g_replace_count = g_replace.size();
}

std::shared_ptr<const Replacement> FindReplacement(uint32_t data) {
    if (!g_replace_count) return nullptr;
    std::lock_guard<std::mutex> lock(g_replace_lock);
    auto it = g_replace.find(data);
    return it == g_replace.end() ? nullptr : it->second;
}

std::mutex g_sharp_lock;
std::unordered_map<uint32_t, int> g_sharp_drops;  // sharpened pictures dropped, by texture

bool ReplacementHolds(const Replacement& r) {
    if (Hash(kernel::HostPtr(r.data), r.guest_bytes) == r.guest_hash) return true;
    if (r.sharpened) {
        std::lock_guard<std::mutex> lock(g_sharp_lock);
        g_sharp_drops[r.data]++;
    }
    std::lock_guard<std::mutex> lock(g_replace_lock);
    auto it = g_replace.find(r.data);
    if (it != g_replace.end() && it->second->version == r.version) {
        g_replace.erase(it);
        g_replace_count = g_replace.size();
        LOGR("gpu: the picture for texture %08X dropped, the game's bytes there changed", r.data);
    }
    return false;
}

std::vector<uint8_t> UpscaleMask(const uint8_t* alpha, uint32_t width, uint32_t height, uint32_t scale) {
    const int w = int(width), h = int(height), s = int(scale);
    auto at = [&](int x, int y) { return alpha[size_t(std::clamp(y, 0, h - 1)) * width + size_t(std::clamp(x, 0, w - 1))] / 255.0f; };
    auto cubic = [](float t, float* k) {  // Catmull-Rom weights for the four taps around t
        float t2 = t * t, t3 = t2 * t;
        k[0] = -0.5f * t3 + t2 - 0.5f * t;
        k[1] = 1.5f * t3 - 2.5f * t2 + 1;
        k[2] = -1.5f * t3 + 2 * t2 + 0.5f * t;
        k[3] = 0.5f * t3 - 0.5f * t2;
    };
    // the interpolated field at output pixel centres, a pixel of margin for the gradient
    const int ow = w * s, oh = h * s;
    std::vector<float> f(size_t(ow + 2) * size_t(oh + 2));
    for (int y = -1; y <= oh; y++) {
        float v = (y + 0.5f) / s - 0.5f;
        int y0 = int(std::floor(v));
        float ky[4];
        cubic(v - y0, ky);
        for (int x = -1; x <= ow; x++) {
            float u = (x + 0.5f) / s - 0.5f;
            int x0 = int(std::floor(u));
            float kx[4];
            cubic(u - x0, kx);
            float sum = 0;
            for (int j = 0; j < 4; j++) {
                float row = 0;
                for (int i = 0; i < 4; i++) row += kx[i] * at(x0 - 1 + i, y0 - 1 + j);
                sum += ky[j] * row;
            }
            f[size_t(y + 1) * size_t(ow + 2) + size_t(x + 1)] = sum;
        }
    }
    std::vector<uint8_t> out(size_t(ow) * size_t(oh));
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            auto F = [&](int dx, int dy) { return f[size_t(y + 1 + dy) * size_t(ow + 2) + size_t(x + 1 + dx)]; };
            float c = F(0, 0), gx = (F(1, 0) - F(-1, 0)) / 2, gy = (F(0, 1) - F(0, -1)) / 2;
            float g = std::sqrt(gx * gx + gy * gy);  // per output pixel
            float a = g > 1e-4f ? (c - 0.5f) / g + 0.5f : (c >= 0.5f ? 1.0f : 0.0f);
            out[size_t(y) * size_t(ow) + size_t(x)] = uint8_t(std::clamp(a, 0.0f, 1.0f) * 255 + 0.5f);
        }
    return out;
}

namespace {
// DXT1/3/5 blocks (Host BC1 BC2 BC3 rows from DecodeLevel) to B8G8R8A8
void Unpack565(uint16_t c, uint8_t* o) {
    o[0] = uint8_t((c & 31) * 255 / 31);
    o[1] = uint8_t((c >> 5 & 63) * 255 / 63);
    o[2] = uint8_t((c >> 11) * 255 / 31);
}
void DecodeBlocks(Host host, uint32_t w, uint32_t h, const std::vector<uint8_t>& in, uint32_t pitch,
                  std::vector<uint32_t>& out) {
    out.assign(size_t(w) * h, 0);
    const uint32_t block = host == Host::BC1 ? 8 : 16;
    for (uint32_t by = 0; by < (h + 3) / 4; by++)
        for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
            size_t at = size_t(by) * pitch + size_t(bx) * block;
            if (at + block > in.size()) continue;
            const uint8_t* b = &in[at];
            const uint8_t* c = b + (block - 8);
            uint16_t c0 = uint16_t(c[0] | c[1] << 8), c1 = uint16_t(c[2] | c[3] << 8);
            uint8_t pal[4][4] = {};
            Unpack565(c0, pal[0]);
            Unpack565(c1, pal[1]);
            bool four = host != Host::BC1 || c0 > c1;
            for (int i = 0; i < 3; i++) {
                pal[2][i] = uint8_t(four ? (2 * pal[0][i] + pal[1][i]) / 3 : (pal[0][i] + pal[1][i]) / 2);
                pal[3][i] = uint8_t(four ? (pal[0][i] + 2 * pal[1][i]) / 3 : 0);
            }
            pal[0][3] = pal[1][3] = pal[2][3] = 255;
            pal[3][3] = four ? 255 : 0;
            uint32_t idx = uint32_t(c[4] | c[5] << 8 | c[6] << 16 | c[7] << 24);
            uint8_t alpha[16];
            for (int k = 0; k < 16; k++) alpha[k] = 255;
            if (host == Host::BC2) {
                for (int k = 0; k < 16; k++) alpha[k] = uint8_t((b[k / 2] >> (k % 2 * 4) & 15) * 17);
            } else if (host == Host::BC3) {
                uint8_t a[8] = {b[0], b[1]};
                for (int i = 2; i < 8; i++)
                    a[i] = uint8_t(b[0] > b[1] ? ((8 - i) * b[0] + (i - 1) * b[1]) / 7
                                   : i < 6     ? ((6 - i) * b[0] + (i - 1) * b[1]) / 5
                                   : i == 6    ? 0
                                               : 255);
                uint64_t bits = 0;
                for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
                for (int k = 0; k < 16; k++) alpha[k] = a[bits >> (3 * k) & 7];
            }
            for (int k = 0; k < 16; k++) {
                uint32_t x = bx * 4 + uint32_t(k % 4), y = by * 4 + uint32_t(k / 4);
                if (x >= w || y >= h) continue;
                const uint8_t* p = pal[idx >> (2 * k) & 3];
                uint8_t a = host == Host::BC1 ? p[3] : alpha[k];
                out[size_t(y) * w + x] = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(a) << 24;
            }
        }
}

float Cubic(float t, int i) {  // Catmull-Rom weight of tap i (0..3) around t
    float t2 = t * t, t3 = t2 * t;
    switch (i) {
        case 0: return -0.5f * t3 + t2 - 0.5f * t;
        case 1: return 1.5f * t3 - 2.5f * t2 + 1;
        case 2: return -1.5f * t3 + 2 * t2 + 0.5f * t;
        default: return 0.5f * t3 - 0.5f * t2;
    }
}
}  // namespace

bool DecodeBgra(const TextureDesc& d, uint32_t level, const uint8_t* guest, std::vector<uint32_t>& out) {
    const FormatInfo& fi = Format(d.format);
    if (fi.depth || fi.host == Host::None || fi.host == Host::R32F || fi.host == Host::R16Unorm) return false;
    std::vector<uint8_t> px;
    uint32_t pitch = 0;
    DecodeLevel(d, level, 0, guest, nullptr, px, pitch);
    uint32_t w = std::max(1u, d.width >> level), h = std::max(1u, d.height >> level);
    if (fi.compressed) {
        DecodeBlocks(fi.host, w, h, px, pitch, out);
        return true;
    }
    if (pitch < w * 4 || px.size() < size_t(pitch) * h) return false;
    out.resize(size_t(w) * h);
    for (uint32_t y = 0; y < h; y++) std::memcpy(&out[size_t(y) * w], &px[size_t(y) * pitch], size_t(w) * 4);
    return true;
}

std::vector<uint32_t> UpscalePicture(const std::vector<uint32_t>& bgra, uint32_t width, uint32_t height, uint32_t scale) {
    const size_t n = size_t(width) * height;
    std::vector<uint8_t> alpha(n);
    size_t seen = 0, partial = 0;
    for (size_t i = 0; i < n; i++) {
        alpha[i] = uint8_t(bgra[i] >> 24);
        seen += alpha[i] > 8;
        partial += alpha[i] > 8 && alpha[i] < 247;
    }
    const bool mask = seen && partial * 4 < seen;  // edges, not glows
    const int w = int(width), h = int(height), s = int(scale), ow = w * s, oh = h * s;
    std::vector<uint8_t> a = mask ? UpscaleMask(alpha.data(), width, height, scale) : std::vector<uint8_t>();
    std::vector<uint32_t> out(size_t(ow) * size_t(oh));
    auto tap = [&](int x, int y) { return bgra[size_t(std::clamp(y, 0, h - 1)) * width + size_t(std::clamp(x, 0, w - 1))]; };
    for (int y = 0; y < oh; y++) {
        float v = (y + 0.5f) / s - 0.5f;
        int y0 = int(std::floor(v));
        float ky[4];
        for (int j = 0; j < 4; j++) ky[j] = Cubic(v - y0, j);
        for (int x = 0; x < ow; x++) {
            float u = (x + 0.5f) / s - 0.5f;
            int x0 = int(std::floor(u));
            float kx[4];
            for (int i = 0; i < 4; i++) kx[i] = Cubic(u - x0, i);
            float sum[4] = {}, plain[3] = {}, wsum = 0;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) {
                    uint32_t c = tap(x0 - 1 + i, y0 - 1 + j);
                    float k = kx[i] * ky[j], ca = float(c >> 24) / 255;
                    for (int ch = 0; ch < 3; ch++) {
                        sum[ch] += k * ca * float(c >> (8 * ch) & 255);
                        plain[ch] += k * float(c >> (8 * ch) & 255);
                    }
                    sum[3] += k * ca;
                    wsum += k;
                }
            float al = std::clamp(sum[3] / wsum, 0.0f, 1.0f);
            uint32_t o = 0;
            for (int ch = 0; ch < 3; ch++) {
                // the colour where it shows, weighted by alpha so clear texels lend none of theirs
                float c = sum[3] > 1e-3f ? sum[ch] / sum[3] : plain[ch] / wsum;
                o |= uint32_t(std::clamp(c, 0.0f, 255.0f) + 0.5f) << (8 * ch);
            }
            uint32_t oa = mask ? a[size_t(y) * size_t(ow) + size_t(x)] : uint32_t(al * 255 + 0.5f);
            out[size_t(y) * size_t(ow) + size_t(x)] = o | oa << 24;
        }
    }
    return out;
}

namespace {
struct SharpJob {
    TextureDesc d;
    std::vector<uint8_t> bytes;  // level 0 as the game had it
};
std::condition_variable g_sharp_cv;
std::deque<SharpJob> g_sharp_jobs;
std::unordered_map<uint32_t, int> g_sharp_queued;  // queued or done, by texture: the drops it had then

void SharpWorker() {
    for (;;) {
        SharpJob job;
        {
            std::unique_lock<std::mutex> lock(g_sharp_lock);
            g_sharp_cv.wait(lock, [] { return !g_sharp_jobs.empty(); });
            job = std::move(g_sharp_jobs.front());
            g_sharp_jobs.pop_front();
        }
        std::vector<uint32_t> px;
        if (!DecodeBgra(job.d, 0, job.bytes.data(), px)) continue;
        // text (alpha only) and small pictures four times, larger ones twice: a 256 square picture at four times
        // would be 4 MB here and 5 MB of video memory
        uint32_t big = std::max(job.d.width, job.d.height);
        bool text = job.d.format == 0x19 || job.d.format == 0x1F;  // A8
        uint32_t scale = big <= 128 || (text && big <= 512) ? 4 : 2;
        auto r = std::make_shared<Replacement>();
        r->data = job.d.data;
        r->guest_bytes = uint32_t(job.bytes.size());
        r->guest_hash = Hash(job.bytes.data(), job.bytes.size());
        r->width = job.d.width * scale;
        r->height = job.d.height * scale;
        r->sharpened = true;
        r->bgra = UpscalePicture(px, job.d.width, job.d.height, scale);
        if (auto had = FindReplacement(job.d.data); had && !had->sharpened) continue;  // the title's own stays
        // XBE_SHARPEN_DUMP=folder: each picture as a TGA, the game's (orig_<texture>.tga) and enlarged (sharp_)
        if (const char* dir = std::getenv("XBE_SHARPEN_DUMP")) {
            auto tga = [&](const char* name, uint32_t w, uint32_t h, const std::vector<uint32_t>& px) {
                std::string path = std::string(dir) + "/" + name + std::to_string(job.d.data) + ".tga";
                if (FILE* f = std::fopen(path.c_str(), "wb")) {
                    uint8_t head[18] = {0, 0, 2};
                    head[12] = uint8_t(w), head[13] = uint8_t(w >> 8);
                    head[14] = uint8_t(h), head[15] = uint8_t(h >> 8);
                    head[16] = 32, head[17] = 0x28;  // top row first, 8 bits of alpha
                    std::fwrite(head, 1, sizeof head, f);
                    std::fwrite(px.data(), 4, px.size(), f);
                    std::fclose(f);
                }
            };
            tga("orig_", job.d.width, job.d.height, px);
            tga("sharp_", r->width, r->height, r->bgra);
        }
        RegisterReplacement(std::move(r));
        static std::atomic<int> told{0};
        if (told++ < 8)
            LOGI("gpu: texture %08X (%s %ux%u) sharpened %u times for 2D", job.d.data, Format(job.d.format).name,
                 job.d.width, job.d.height, scale);
    }
}
}  // namespace

void Sharpen2D(const TextureDesc& d) {
    if (!app::kGame.sharpen_2d || !app::Get().sharp_2d) return;
    if (d.linear || d.cube || d.depth > 1 || d.format == 0x0B) return;  // P8: its colours come with the palette bound
    if (d.width < 8 || d.height < 8 || d.width > 512 || d.height > 512) return;
    const FormatInfo& fi = Format(d.format);
    if (fi.depth || fi.host == Host::None || fi.host == Host::R32F || fi.host == Host::R16Unorm) return;
    std::lock_guard<std::mutex> lock(g_sharp_lock);
    // made again after its bytes changed once (a level loaded over it); a texture changing more is left as it is
    int drops = g_sharp_drops[d.data];
    if (drops >= 2) return;
    auto [it, fresh] = g_sharp_queued.emplace(d.data, drops);
    if (!fresh && it->second == drops) return;
    it->second = drops;
    static std::once_flag started;
    std::call_once(started, [] { std::thread(SharpWorker).detach(); });
    SharpJob job{d, {}};
    uint32_t size = LevelSize(d, 0);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
    job.bytes.assign(p, p + size);
    g_sharp_jobs.push_back(std::move(job));
    g_sharp_cv.notify_one();
}

std::vector<std::vector<uint32_t>> ReplacementLevels(const Replacement& r) {
    std::vector<std::vector<uint32_t>> levels{r.bgra};
    uint32_t w = r.width, h = r.height;
    while (w > 1 || h > 1) {
        uint32_t nw = std::max(w / 2, 1u), nh = std::max(h / 2, 1u);
        const std::vector<uint32_t>& up = levels.back();
        std::vector<uint32_t> down(size_t(nw) * nh);
        for (uint32_t y = 0; y < nh; y++)
            for (uint32_t x = 0; x < nw; x++) {
                uint32_t sum[4] = {};
                for (uint32_t dy = 0; dy < 2; dy++)
                    for (uint32_t dx = 0; dx < 2; dx++) {
                        uint32_t c = up[size_t(std::min(y * 2 + dy, h - 1)) * w + std::min(x * 2 + dx, w - 1)];
                        for (int i = 0; i < 4; i++) sum[i] += c >> (i * 8) & 0xFF;
                    }
                uint32_t c = 0;
                for (int i = 0; i < 4; i++) c |= (sum[i] + 2) / 4 << (i * 8);
                down[size_t(y) * nw + x] = c;
            }
        levels.push_back(std::move(down));
        w = nw;
        h = nh;
    }
    return levels;
}

bool CopySurfaceMemory(const Surface& src, const Surface& dst, uint32_t count, uint32_t rects, uint32_t points) {
    TextureDesc a, b;
    if (!DescribeSurface(src, a) || !DescribeSurface(dst, b)) return false;
    const FormatInfo &fa = Format(a.format), &fb = Format(b.format);
    // DXT copies whole 4x4 blocks, rows of blocks in memory
    bool block = fa.compressed;
    if (block != fb.compressed || (block && a.format != b.format) || fa.bits != fb.bits || fa.bits < 8) return false;
    uint32_t bpp = block ? fa.bits : fa.bits / 8, u = block ? 4 : 1;
    uint32_t ax, ay, az, bx, by, bz;
    SwizzleMasks(a.width, a.height, 1, ax, ay, az);
    SwizzleMasks(b.width, b.height, 1, bx, by, bz);
    auto addr = [&](const TextureDesc& d, uint32_t mx, uint32_t my, uint32_t x, uint32_t y) {
        if (block) return d.data + (y * ((d.width + 3) / 4) + x) * bpp;
        return d.data + (d.linear ? y * d.pitch + x * bpp : (Deposit(x, mx) | Deposit(y, my)) * bpp);
    };
    int32_t aw = int32_t((a.width + u - 1) / u), ah = int32_t((a.height + u - 1) / u);
    int32_t bw = int32_t((b.width + u - 1) / u), bh = int32_t((b.height + u - 1) / u);
    uint32_t n = count && rects ? count : 1;
    for (uint32_t i = 0; i < n; i++) {
        int32_t x0 = 0, y0 = 0, x1 = int32_t(a.width), y1 = int32_t(a.height), dx = 0, dy = 0;
        if (count && rects) {
            uint32_t r = rects + i * 16;
            x0 = int32_t(kernel::R32(r));
            y0 = int32_t(kernel::R32(r + 4));
            x1 = int32_t(kernel::R32(r + 8));
            y1 = int32_t(kernel::R32(r + 12));
            dx = x0;
            dy = y0;
        }
        if (points) {
            dx = int32_t(kernel::R32(points + i * 8));
            dy = int32_t(kernel::R32(points + i * 8 + 4));
        }
        int32_t U = int32_t(u);
        x0 /= U, y0 /= U, dx /= U, dy /= U, x1 = (x1 + U - 1) / U, y1 = (y1 + U - 1) / U;
        for (int32_t y = std::max(y0, 0); y < std::min(y1, ah); y++)
            for (int32_t x = std::max(x0, 0); x < std::min(x1, aw); x++) {
                int32_t tx = dx + x - x0, ty = dy + y - y0;
                if (tx < 0 || ty < 0 || tx >= bw || ty >= bh) continue;
                std::memcpy(kernel::HostPtr(addr(b, bx, by, uint32_t(tx), uint32_t(ty))),
                            kernel::HostPtr(addr(a, ax, ay, uint32_t(x), uint32_t(y))), bpp);
            }
    }
    MarkDirty(dst.header);
    return true;
}

// declarations

static uint32_t TypeSize(uint32_t t) {
    uint32_t kind = t & 15, count = (t >> 4) & 15;
    switch (kind) {
        case 0: return 4;                // D3DCOLOR
        case 1: case 5: return 2 * count;  // shorts
        case 2: return 4 * count;        // floats
        case 4: return count;            // bytes
        case 6: return 4;                // packed 11:11:10
        default: return 4 * count;
    }
}

void ParseDeclaration(VertexShader& vs) {
    int stream = 0;
    uint32_t offset = 0;
    vs.stream_stride.fill(0);
    for (size_t i = 0; i < vs.decl.size(); i++) {
        uint32_t t = vs.decl[i];
        if (t == 0xFFFFFFFF) break;  // D3DVSD_END
        uint32_t kind = t >> 29;
        if (kind == 1) {  // STREAM
            if (t & (1u << 28)) continue;  // tessellator stream
            vs.stream_stride[size_t(stream)] = std::max(vs.stream_stride[size_t(stream)], offset);
            stream = int(t & 15);
            offset = vs.stream_stride[size_t(stream)];
        } else if (kind == 2) {  // STREAMDATA
            if (t & (1u << 28)) {
                // SKIP counts dwords, SKIPBYTES bytes
                uint32_t n = (t >> 16) & 0xFF;
                offset += (t & (1u << 27)) ? n : n * 4;
            } else {
                uint32_t reg = t & 15, type = (t >> 16) & 0xFF;
                if (type != 0x02) vs.inputs[reg] = {stream, offset, type};  // 0x02 D3DVSDT_NONE
                offset += type == 0x02 ? 0 : TypeSize(type);
            }
        } else if (kind == 4) {  // CONSTMEM
            uint32_t count = (t >> 25) & 15, reg = t & 0xFF;
            for (uint32_t c = 0; c < count && i + 4 < vs.decl.size(); c++) {
                std::array<float, 4> v;
                std::memcpy(v.data(), &vs.decl[i + 1], 16);
                vs.constants.push_back({reg + c, v});
                i += 4;
            }
        }
    }
    vs.stream_stride[size_t(stream)] = std::max(vs.stream_stride[size_t(stream)], offset);
}

// pixel shader constants

uint32_t PackColor(const std::array<float, 4>& c) {
    auto q = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return q(c[3]) << 24 | q(c[0]) << 16 | q(c[1]) << 8 | q(c[2]);
}

// PSC0Mapping and PSC1Mapping give each combiner stage the D3D constant its C0 and C1 read,
// PSFinalCombinerConstants does the same for the final combiner. 0xF leaves the literal in place.
void ApplyPixelConstants(State& s, uint32_t first, uint32_t count) {
    if (!s.ps) return;
    const uint32_t* d = s.ps->def;
    uint32_t c0map = d[57], c1map = d[58], fmap = d[59];
    auto mine = [&](uint32_t m) { return m >= first && m < first + count && m < 8; };
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t m0 = (c0map >> (i * 4)) & 0xF, m1 = (c1map >> (i * 4)) & 0xF;
        if (mine(m0)) s.simple[(kMPsConstant0 >> 2) + i] = PackColor(s.psc[m0]);
        if (mine(m1)) s.simple[(kMPsConstant1 >> 2) + i] = PackColor(s.psc[m1]);
    }
    uint32_t f0 = fmap & 0xF, f1 = (fmap >> 4) & 0xF;
    if (mine(f0)) s.simple[kMPsFinalConstant0 >> 2] = PackColor(s.psc[f0]);
    if (mine(f1)) s.simple[kMPsFinalConstant1 >> 2] = PackColor(s.psc[f1]);
}

}  // namespace gpu
