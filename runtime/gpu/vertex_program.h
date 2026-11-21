// Xbox vertex programs run on the CPU, one vertex at a time: the reference renderer's (harness) and the renderers'
// look at where a program puts a draw (gpu::ScreenExtent, for State::hud_layout)
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "gpu/state.h"

namespace gpu::vp {

struct Vec {
    float c[4] = {};
    float& operator[](int i) { return c[i]; }
    float operator[](int i) const { return c[i]; }
};
inline Vec V(float x, float y, float z, float w) { return {{x, y, z, w}}; }

// the microcode fields, as shaders.cpp names them
struct Tok {
    const uint32_t* t;
    uint32_t f(int sub, int bit, int len) const { return (t[sub] >> bit) & ((1u << len) - 1); }
};

// magnitude clamped between 1 / 2^64 and 2^64, sign kept
inline float MagClamp(float v) {
    float m = std::clamp(std::fabs(v), 5.42101e-20f, 1.84467e+19f);
    return v < 0 ? -m : m;
}
inline float MacMul(float a, float b) { return (a == 0 || b == 0) ? 0 : a * b; }
inline Vec Splat(float v) { return V(v, v, v, v); }

// 1 mov, 2 mul, 3 add, 4 mad, 5 dp3, 6 dph, 7 dp4, 8 dst, 9 min, 10 max, 11 slt, 12 sge, 13 arl
inline Vec Mac(uint32_t op, const Vec& a, const Vec& b, const Vec& c) {
    Vec r;
    switch (op) {
        case 1:
        case 13: return a;
        case 2: for (int i = 0; i < 4; i++) r[i] = MacMul(a[i], b[i]); return r;
        case 3: for (int i = 0; i < 4; i++) r[i] = a[i] + c[i]; return r;
        case 4: for (int i = 0; i < 4; i++) r[i] = MacMul(a[i], b[i]) + c[i]; return r;
        case 5: return Splat(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
        case 6: return Splat(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + b[3]);
        case 7: return Splat(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
        case 8: return V(1, a[1] * b[1], a[2], b[3]);
        case 9: for (int i = 0; i < 4; i++) r[i] = std::min(a[i], b[i]); return r;
        case 10: for (int i = 0; i < 4; i++) r[i] = std::max(a[i], b[i]); return r;
        case 11: for (int i = 0; i < 4; i++) r[i] = a[i] < b[i] ? 1.f : 0.f; return r;
        case 12: for (int i = 0; i < 4; i++) r[i] = a[i] >= b[i] ? 1.f : 0.f; return r;
        default: return r;
    }
}

// 1 mov, 2 rcp, 3 rcc, 4 rsq, 5 exp, 6 log, 7 lit
inline Vec Ilu(uint32_t op, const Vec& c) {
    float v = c[0];
    switch (op) {
        case 1: return c;
        case 2: return Splat(1.0f / v);
        case 3: return Splat(MagClamp(1.0f / v));
        case 4: return Splat(v == 0 ? INFINITY : 1.0f / std::sqrt(std::fabs(v)));
        case 5: {
            float whole = std::floor(v);
            return V(std::exp2(whole), v - whole, std::exp2(v), 1);
        }
        case 6: {
            float mag = std::fabs(v);
            if (mag == 0) return V(-INFINITY, 1, -INFINITY, 1);
            float ex = std::floor(std::log2(mag));
            return V(ex, mag / std::exp2(ex), std::log2(mag), 1);
        }
        case 7: {
            const float cap = 127.99609375f;
            float power = std::clamp(c[3], -cap, cap), diffuse = std::max(c[0], 0.f), spec = std::max(c[1], 0.f);
            return V(1, diffuse, diffuse > 0 ? std::exp2(power * std::log2(spec)) : 0, 1);
        }
        default: return Vec{};
    }
}

struct VertexProgram {
    const State& s;
    Vec in[16];
    Vec temp[12], out[16];  // out 0 oPos, 3 oD0, 4 oD1, 5 oFog, 6 oPts, 7 oB0, 8 oB1, 9 to 12 oT0 to oT3
    int a0 = 0;

    // temp 12 is oPos
    Vec& Reg(uint32_t r) { return r == 12 ? out[0] : temp[r]; }

    // operand A 0, B 1, C 2
    Vec Operand(const Tok& k, int which) {
        uint32_t neg, sw[4], reg, mux;
        if (which == 0) {
            neg = k.f(1, 8, 1);
            sw[0] = k.f(1, 6, 2), sw[1] = k.f(1, 4, 2), sw[2] = k.f(1, 2, 2), sw[3] = k.f(1, 0, 2);
            reg = k.f(2, 28, 4), mux = k.f(2, 26, 2);
        } else if (which == 1) {
            neg = k.f(2, 25, 1);
            sw[0] = k.f(2, 23, 2), sw[1] = k.f(2, 21, 2), sw[2] = k.f(2, 19, 2), sw[3] = k.f(2, 17, 2);
            reg = k.f(2, 13, 4), mux = k.f(2, 11, 2);
        } else {
            neg = k.f(2, 10, 1);
            sw[0] = k.f(2, 8, 2), sw[1] = k.f(2, 6, 2), sw[2] = k.f(2, 4, 2), sw[3] = k.f(2, 2, 2);
            reg = k.f(2, 0, 2) << 2 | k.f(3, 30, 2), mux = k.f(3, 28, 2);
            uint32_t ilu = k.f(1, 25, 3);
            if (ilu >= 2 && ilu <= 6) sw[1] = sw[2] = sw[3] = sw[0];  // scalar ops read x
        }
        Vec base;
        if (mux == 1) {
            base = reg == 12 ? out[0] : temp[reg > 12 ? 0 : reg];
        } else if (mux == 2) {
            base = in[k.f(1, 9, 4)];
        } else {
            int cr = int(k.f(1, 13, 8));
            if (k.f(3, 1, 1)) cr = std::clamp(cr + a0, 0, 191);
            const auto& c = s.vsc[size_t(cr)];
            base = V(c[0], c[1], c[2], c[3]);
        }
        Vec r;
        for (int i = 0; i < 4; i++) r[i] = neg ? -base[int(sw[i])] : base[int(sw[i])];
        return r;
    }

    static void Masked(Vec& dst, const Vec& src, uint32_t xyzw) {
        for (int i = 0; i < 4; i++)
            if (xyzw >> (3 - i) & 1) dst[i] = src[i];
    }

    void Run(const std::vector<uint32_t>& prog) {
        out[3] = out[7] = out[9] = out[10] = out[11] = out[12] = V(0, 0, 0, 1);
        out[6] = V(1, 1, 1, 1);
        for (size_t n = 0; n + 4 <= prog.size(); n += 4) {
            Tok k{&prog[n]};
            uint32_t mac = k.f(1, 21, 4), ilu = k.f(1, 25, 3);
            if (mac || ilu) {
                Vec a = Operand(k, 0), b = Operand(k, 1), c = Operand(k, 2);
                Vec mr = mac ? Mac(mac, a, b, c) : Vec{}, ir = ilu ? Ilu(ilu, c) : Vec{};
                uint32_t r = k.f(3, 20, 4), omask = k.f(3, 12, 4);
                bool use_ilu = k.f(3, 2, 1) == 1;
                // the output register is written before the temporaries
                if (omask && k.f(3, 11, 1) && ((use_ilu && ilu) || (!use_ilu && mac && mac != 13)))
                    Masked(out[k.f(3, 3, 8) & 0xF], use_ilu ? ir : mr, omask);
                if (ilu) {
                    uint32_t dst = mac ? 1 : r;
                    if (dst <= 12) Masked(Reg(dst), ir, k.f(3, 16, 4));
                }
                if (mac == 13) {
                    a0 = int(std::floor(mr[0] + 0.001f));
                } else if (mac) {
                    uint32_t m = ilu && r == 1 ? 0 : k.f(3, 24, 4);
                    if (r <= 12) Masked(Reg(r), mr, m);
                }
            }
            if (k.f(3, 0, 1)) break;
        }
    }
};

}  // namespace gpu::vp
