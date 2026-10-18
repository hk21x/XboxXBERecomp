#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>

// diagnostics switched on by environment variables, read once
namespace diag {

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
