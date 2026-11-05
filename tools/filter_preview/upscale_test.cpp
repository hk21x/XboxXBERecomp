// UpscaleEdgeDirected2x against the truth: a picture halved, enlarged again by it and by a plain cubic, each compared with
// the original.   upscale_test in.ppm out_prefix     writes <out_prefix>_cubic.ppm, _edge.ppm; fails if it is further off
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include "gpu/upscale.h"
int main(int argc, char** argv) {
    std::ifstream f(argv[1], std::ios::binary);
    std::string m;
    int W, H, mx;
    f >> m >> W >> H >> mx;
    f.get();
    std::vector<unsigned char> b(size_t(W) * H * 3);
    f.read(reinterpret_cast<char*>(b.data()), std::streamsize(b.size()));
    W &= ~1, H &= ~1;
    int w = W / 2, h = H / 2, full = 0;
    { std::ifstream g(argv[1], std::ios::binary); g >> m >> full; }
    auto px = [&](int x, int y) { const unsigned char* p = &b[(size_t(y) * full + x) * 3]; return uint32_t(p[2]) | uint32_t(p[1]) << 8 | uint32_t(p[0]) << 16 | 0xFF000000u; };
    std::vector<uint32_t> small(size_t(w) * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) small[size_t(y) * w + x] = px(x * 2, y * 2);  // no blur first: the edges to save are stepped
    auto edge = gpu::UpscaleEdgeDirected2x(small, w, h, false, false);
    std::vector<uint32_t> cubic(size_t(W) * H);
    auto cub = [](float a, float b, float c, float d) { return std::clamp((-a + 9 * b + 9 * c - d) / 16, 0.f, 255.f); };
    auto s = [&](int x, int y, int ch) { return float(small[size_t(std::clamp(y, 0, h - 1)) * w + std::clamp(x, 0, w - 1)] >> (8 * ch) & 255); };
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int X = x / 2, Y = y / 2;
            uint32_t o = 0xFF000000u;
            for (int ch = 0; ch < 3; ch++) {
                auto row = [&](int yy) { return x & 1 ? cub(s(X - 1, yy, ch), s(X, yy, ch), s(X + 1, yy, ch), s(X + 2, yy, ch)) : s(X, yy, ch); };
                float v = y & 1 ? cub(row(Y - 1), row(Y), row(Y + 1), row(Y + 2)) : row(Y);
                o |= uint32_t(v + 0.5f) << (8 * ch);
            }
            cubic[size_t(y) * W + x] = o;
        }
    auto score = [&](const std::vector<uint32_t>& a) {  // the error over the new pixels, as PSNR
        double e = 0; size_t n = 0;
        for (int y = 2; y < H - 2; y++)
            for (int x = 2; x < W - 2; x++) {
                if (!(x & 1) && !(y & 1)) continue;
                uint32_t p = a[size_t(y) * W + x], q = px(x, y);
                for (int ch = 0; ch < 3; ch++) { double d = double(p >> (8 * ch) & 255) - double(q >> (8 * ch) & 255); e += d * d; n++; }
            }
        return 10 * std::log10(255.0 * 255.0 / (e / double(n)));
    };
    auto write = [&](const std::string& p, const std::vector<uint32_t>& a) {
        std::ofstream o(p, std::ios::binary);
        o << "P6\n" << W << " " << H << "\n255\n";
        for (uint32_t c : a) { char q[3] = {char(c >> 16), char(c >> 8), char(c)}; o.write(q, 3); }
    };
    write(std::string(argv[2]) + "_cubic.ppm", cubic), write(std::string(argv[2]) + "_edge.ppm", edge);
    double a = score(cubic), e = score(edge);
    // the original pixels stay as they were
    int moved = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) moved += edge[size_t(y * 2) * W + x * 2] != small[size_t(y) * w + x];
    bool ok = e > a - 0.1 && !moved;  // frames are aliased, so no better than cubic by this measure is fine; textures are the aim
    std::printf("%s: cubic %.2f dB, edge directed %.2f dB (%+.2f), %d originals moved: %s\n", argv[1], a, e, e - a, moved, ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
