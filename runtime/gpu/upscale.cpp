#include "gpu/upscale.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace gpu {

namespace {

using Px = std::array<float, 4>;

Px Unpack(uint32_t c) {
    return {float(c & 255), float(c >> 8 & 255), float(c >> 16 & 255), float(c >> 24)};
}
uint32_t Pack(const Px& p) {
    uint32_t c = 0;
    for (int i = 0; i < 4; i++) c |= uint32_t(std::clamp(p[size_t(i)], 0.0f, 255.0f) + 0.5f) << (8 * i);
    return c;
}
float Luma(const Px& p) { return 0.299f * p[2] + 0.587f * p[1] + 0.114f * p[0]; }

// the cubic through four pixels in a line, at their middle
Px Mid(const Px& a, const Px& b, const Px& c, const Px& d) {
    Px o;
    for (size_t i = 0; i < 4; i++) o[i] = (-a[i] + 9 * b[i] + 9 * c[i] - d[i]) / 16;
    // no overshoot past the two it lies between by more than a little: halos on hard edges otherwise
    for (size_t i = 0; i < 4; i++) {
        float lo = std::min(b[i], c[i]), hi = std::max(b[i], c[i]), slack = (hi - lo) * 0.1f;
        o[i] = std::clamp(o[i], lo - slack, hi + slack);
    }
    return o;
}
Px Mix(const Px& a, const Px& b, float k) {
    Px o;
    for (size_t i = 0; i < 4; i++) o[i] = a[i] * (1 - k) + b[i] * k;
    return o;
}

}  // namespace

std::vector<uint32_t> UpscaleEdgeDirected2x(const std::vector<uint32_t>& bgra, uint32_t width, uint32_t height, bool wrap_u, bool wrap_v) {
    const int W = int(width) * 2, H = int(height) * 2;
    std::vector<Px> o(size_t(W) * size_t(H));
    // positions on the doubled lattice: past an edge they wrap, or stay on the edge's own pixels (the even lattice)
    auto fold = [](int v, int size, bool wrap, bool even_only) {
        if (wrap) return (v % size + size) % size;
        int last = even_only ? (size - 2) | (v & 1) : size - 1;
        return std::clamp(v, v & 1, last);
    };
    auto at = [&](int x, int y) -> const Px& { return o[size_t(fold(y, H, wrap_v, true)) * size_t(W) + size_t(fold(x, W, wrap_u, true))]; };
    for (int y = 0; y < int(height); y++)
        for (int x = 0; x < int(width); x++) o[size_t(y * 2) * size_t(W) + size_t(x * 2)] = Unpack(bgra[size_t(y) * width + size_t(x)]);
    // one new pixel from its four nearest and the lines through them: dx, dy step along one direction, the other at a
    // right angle to it; the smoother direction is the edge's
    auto fill = [&](int x, int y, int dx, int dy) {
        float along = 0, across = 0;
        // differences in each direction around the nine places of this pixel's kind, whose neighbours are all known
        for (int k = -1; k <= 1; k++)
            for (int m = -1; m <= 1; m++) {
                int qx = x + 2 * (m * dx + k * dy), qy = y + 2 * (m * dy - k * dx);
                along += std::abs(Luma(at(qx + dx, qy + dy)) - Luma(at(qx - dx, qy - dy)));
                across += std::abs(Luma(at(qx + dy, qy - dx)) - Luma(at(qx - dy, qy + dx)));
            }
        Px a = Mid(at(x - 3 * dx, y - 3 * dy), at(x - dx, y - dy), at(x + dx, y + dy), at(x + 3 * dx, y + 3 * dy));
        Px b = Mid(at(x - 3 * dy, y + 3 * dx), at(x - dy, y + dx), at(x + dy, y - dx), at(x + 3 * dy, y - 3 * dx));
        // only a clear edge picks a direction: on noise (a road's grain) it would leave lone pixels standing, so there
        // both directions count alike, a plain smooth enlargement
        constexpr float kClearEdge = 90;  // summed over the nine places, in 8 bit luma
        float w = 0.5f;  // of a
        float strong = std::max(along, across), weak = std::min(along, across);
        if (strong > kClearEdge) w = std::clamp((across - along) / (strong + weak) * 2.5f + 0.5f, 0.0f, 1.0f);
        o[size_t(y) * size_t(W) + size_t(x)] = Mix(b, a, w);
    };
    // the centres between four pixels, along the diagonals; then the rest, along the rows and columns
    for (int y = 1; y < H; y += 2)
        for (int x = 1; x < W; x += 2) fill(x, y, 1, 1);
    for (int y = 0; y < H; y++)
        for (int x = (y + 1) & 1; x < W; x += 2) fill(x, y, 1, 0);
    std::vector<uint32_t> out(o.size());
    for (size_t i = 0; i < o.size(); i++) out[i] = Pack(o[i]);
    return out;
}

}  // namespace gpu
