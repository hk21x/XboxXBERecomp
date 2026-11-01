// The finished frame's scaling filter and FXAA with no GPU: their real HLSL (runtime/gpu/d3d_common.cpp, cut out by
// build.sh into final_pass.inc) compiled as C++ on a few small types, run over a picture.
//   filter_preview in.ppm out_prefix output_width output_height     writes <out_prefix>_plain/_sharp/_fxaa.ppm, checks the filter
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
struct float2 { float x, y; float2(float v = 0) : x(v), y(v) {} float2(float a, float b) : x(a), y(b) {} };
struct float3 { float x, y, z; float3(float v = 0) : x(v), y(v), z(v) {} float3(float a, float b, float c) : x(a), y(b), z(c) {} };
#define OPS(T, ...) inline T operator+(T a, T b) { auto f = [](float p, float q) { return p + q; }; return T(__VA_ARGS__); } \
    inline T operator-(T a, T b) { auto f = [](float p, float q) { return p - q; }; return T(__VA_ARGS__); } \
    inline T operator*(T a, T b) { auto f = [](float p, float q) { return p * q; }; return T(__VA_ARGS__); } \
    inline T operator/(T a, T b) { auto f = [](float p, float q) { return p / q; }; return T(__VA_ARGS__); } \
    inline T max(T a, T b) { auto f = [](float p, float q) { return std::max(p, q); }; return T(__VA_ARGS__); } \
    inline T min(T a, T b) { auto f = [](float p, float q) { return std::min(p, q); }; return T(__VA_ARGS__); }
OPS(float2, f(a.x, b.x), f(a.y, b.y))
OPS(float3, f(a.x, b.x), f(a.y, b.y), f(a.z, b.z))
inline float3& operator+=(float3& a, float3 b) { return a = a + b; }
inline float2 operator-(float2 a) { return {-a.x, -a.y}; }
using std::abs;
inline float max(float a, float b) { return std::max(a, b); }
inline float min(float a, float b) { return std::min(a, b); }
inline float saturate(float a) { return std::clamp(a, 0.f, 1.f); }
inline float3 saturate(float3 a) { return {saturate(a.x), saturate(a.y), saturate(a.z)}; }
inline float2 floor(float2 a) { return {std::floor(a.x), std::floor(a.y)}; }
inline float3 sqrt(float3 a) { return {std::sqrt(a.x), std::sqrt(a.y), std::sqrt(a.z)}; }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float2 clamp(float2 a, float lo, float hi) { return {std::clamp(a.x, lo, hi), std::clamp(a.y, lo, hi)}; }

static int W, H;
static std::vector<float3> g_src;
// the renderers' linear clamp sampler
struct Image {
    float3 SampleLevel(int, float2 uv, int) const {
        float fx = uv.x * W - 0.5f, fy = uv.y * H - 0.5f;
        int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
        float ax = fx - x0, ay = fy - y0;
        auto at = [](int x, int y) { return g_src[size_t(std::clamp(y, 0, H - 1)) * W + std::clamp(x, 0, W - 1)]; };
        return (at(x0, y0) * (1 - ax) + at(x0 + 1, y0) * ax) * (1 - ay) + (at(x0, y0 + 1) * (1 - ax) + at(x0 + 1, y0 + 1) * ax) * ay;
    }
};
struct Rgb { float3 v; operator float3() const { return v; } };
static Image img_;
struct Tap { Rgb rgb; };
struct ImgRef { Tap SampleLevel(int s, float2 uv, int l) const { return Tap{Rgb{img_.SampleLevel(s, uv, l)}}; } } img;
static const int ls = 0;
#include "final_pass.inc"

static bool Read(const char* p) {
    std::ifstream f(p, std::ios::binary);
    std::string m;
    int mx;
    if (!(f >> m >> W >> H >> mx) || m != "P6") return false;
    f.get();
    std::vector<unsigned char> b(size_t(W) * H * 3);
    f.read(reinterpret_cast<char*>(b.data()), std::streamsize(b.size()));
    g_src.resize(size_t(W) * H);
    for (size_t i = 0; i < g_src.size(); i++) g_src[i] = {b[i * 3] / 255.f, b[i * 3 + 1] / 255.f, b[i * 3 + 2] / 255.f};
    return true;
}
template <class F> static std::vector<float3> Run(int ow, int oh, F pixel) {
    std::vector<float3> o(size_t(ow) * oh);
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) o[size_t(y) * ow + x] = pixel(float2((x + 0.5f) / ow, (y + 0.5f) / oh));
    return o;
}
static void Write(const std::string& p, const std::vector<float3>& o, int w, int h) {
    std::ofstream f(p, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    for (const float3& c : o) {
        unsigned char b[3] = {(unsigned char)(saturate(c.x) * 255 + 0.5f), (unsigned char)(saturate(c.y) * 255 + 0.5f), (unsigned char)(saturate(c.z) * 255 + 0.5f)};
        f.write(reinterpret_cast<char*>(b), 3);
    }
}
static double Acutance(const std::vector<float3>& o, int w, int h) {  // the mean step between neighbours: higher is sharper
    double s = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x + 1 < w; x++) s += std::abs(luma(o[size_t(y) * w + x]) - luma(o[size_t(y) * w + x + 1]));
    return s / (double(w - 1) * h);
}
int main(int argc, char** argv) {
    if (argc < 5 || !Read(argv[1])) return 2;
    std::string out = argv[2];
    int ow = atoi(argv[3]), oh = atoi(argv[4]);
    float2 t(1.f / W, 1.f / H), px(1.f / ow, 1.f / oh);
    auto plain = Run(ow, oh, [&](float2 uv) { return tap(uv); });
    auto sharp = Run(ow, oh, [&](float2 uv) { return scaled(uv, t, px); });
    auto fx = Run(ow, oh, [&](float2 uv) { return fxaa(scaled(uv, t, px), uv, t); });
    Write(out + "_plain.ppm", plain, ow, oh), Write(out + "_sharp.ppm", sharp, ow, oh), Write(out + "_fxaa.ppm", fx, ow, oh);
    double a = Acutance(plain, ow, oh), b = Acutance(sharp, ow, oh);
    // at the same size the filter must change nothing
    int fail = 0;
    if (ow == W && oh == H)
        for (size_t i = 0; i < plain.size(); i++)
            if (std::abs(plain[i].x - sharp[i].x) > 1e-3f) { fail = 1; break; }
    if (ow != W && b <= a) fail = 1;
    std::printf("%dx%d to %dx%d: step between neighbours plain %.5f, sharp %.5f (%+.1f%%) %s\n", W, H, ow, oh, a, b, (b / a - 1) * 100, fail ? "FAILED" : "OK");
    return fail;
}
