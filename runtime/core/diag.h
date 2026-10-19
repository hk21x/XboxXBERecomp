#pragma once
#include <atomic>
#include <algorithm>
#include <vector>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

// diagnostics switched on by environment variables, read once
namespace diag {

// XBE_FRAMELOG: F4 (or a file named framelog beside the exe) starts the frame log
inline std::atomic<bool> g_framelog_request{false};

// XBE_PERF set: perf log lines once a second and the read-backs named
inline bool Perf() {
    static const bool on = std::getenv("XBE_PERF") != nullptr;
    return on;
}

// XBE_CAPTURE_DIR: where F3 saves a frame, empty: F3 does not capture
inline const std::filesystem::path& CaptureDir() {
    static const std::filesystem::path dir = [] {
#ifdef _WIN32
        const wchar_t* v = _wgetenv(L"XBE_CAPTURE_DIR");
#else
        const char* v = std::getenv("XBE_CAPTURE_DIR");
#endif
        return v ? std::filesystem::path(v) : std::filesystem::path();
    }();
    return dir;
}

// XBE_CAPTURE_DRAWS: a capture also saves the colour target after some draws: "1600-1640" those draw numbers (as
// frame.txt counts them), "vs:B01019E1" every draw with that vertex shader handle (and the target once before the
// first of them)
struct DrawRange {
    unsigned first = 1, last = 0;
    uint32_t vs = 0;
};
inline DrawRange CaptureDraws() {
    static const DrawRange r = [] {
        DrawRange d;
        if (const char* v = std::getenv("XBE_CAPTURE_DRAWS")) {
            unsigned a = 0, b = 0;
            if (std::sscanf(v, "vs:%x", &a) == 1) {
                d.vs = a;
            } else if (int n = std::sscanf(v, "%u-%u", &a, &b); n >= 1) {
                d.first = a;
                d.last = n == 2 ? b : a;
            }
        }
        return d;
    }();
    return r;
}

// XBE_DRAW_TEST="B01019E1:alphakill,cull,depth": an experiment, the draws with that vertex shader handle drawn
// without those tests (alpha kill, back face culling, the depth test); to find which one drops a draw
struct DrawTest {
    uint32_t vs = 0;
    bool alphakill = false, cull = false, depth = false;
};
inline const DrawTest& DrawTests() {
    static const DrawTest t = [] {
        DrawTest d;
        const char* v = std::getenv("XBE_DRAW_TEST");
        unsigned h = 0;
        if (!v || std::sscanf(v, "%x", &h) != 1) return d;
        d.vs = h;
        d.alphakill = std::strstr(v, "alphakill") != nullptr;
        d.cull = std::strstr(v, "cull") != nullptr;
        d.depth = std::strstr(v, "depth") != nullptr;
        return d;
    }();
    return t;
}

// XBE_DRAW_SKIP="B0101C41 B0101931": an experiment, the draws with those vertex shader handles left out (to find
// which layer of a scene flickers)
inline bool SkippedDraw(uint32_t vs) {
    static const std::vector<uint32_t> skip = [] {
        std::vector<uint32_t> v;
        if (const char* e = std::getenv("XBE_DRAW_SKIP"))
            for (const char* p = e; *p;) {
                char* end = nullptr;
                unsigned long h = std::strtoul(p, &end, 16);
                if (end == p) {
                    p++;
                    continue;
                }
                v.push_back(uint32_t(h));
                p = end;
            }
        return v;
    }();
    return !skip.empty() && std::find(skip.begin(), skip.end(), vs) != skip.end();
}

// XBE_VSC="56 54": vertex shader constants (D3D numbering, -96..95) whose writes are logged: each distinct value
// and caller once
inline bool WatchedConstant(int d3d_register) {
    static const std::array<uint64_t, 3> mask = [] {
        std::array<uint64_t, 3> m{};
        const char* v = std::getenv("XBE_VSC");
        for (char* p = const_cast<char*>(v); p && *p;) {
            char* e;
            long r = std::strtol(p, &e, 10) + 96;
            if (e == p) break;
            if (r >= 0 && r < 192) m[r / 64] |= 1ull << (r % 64);
            p = e;
        }
        return m;
    }();
    int r = d3d_register + 96;
    return r >= 0 && r < 192 && (mask[r / 64] >> (r % 64) & 1);
}

}  // namespace diag
