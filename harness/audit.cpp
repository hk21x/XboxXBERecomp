// AUDIT=N: what every Nth frame's draws would ask of a GPU backend, without one
//   cap/hlsl/vs_<key>.hlsl, ps_<key>.hlsl  each shader the D3D12 backend would generate, built as it builds them
//   cap/vertices.txt                       each draw assembled both ways, raw (as D3D12 gets it) and converted on the CPU
//                                           (as the reference renderer gets it), every corner fetched as an input
//                                           assembler fetches it and compared: draws, mismatches, vertex types
//   AUDIT_TEX=<hex address>: that texture as its guest memory decodes (depth as grey), every face of level 0, into
//                                           cap/tex_<address>_<frame>.tga once every 300 frames (between reference frames
//                                           nothing draws into guest memory here, so this is what a GPU backend reads)
//   cap/textures.txt                       each texture sampled: format, size, levels, uses, and how far each mip level
//                                           is from its parent shrunk 2x2 (a wrong level offset reads as noise)
#include "audit.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>

#include <dxgiformat.h>

#include "app/app.h"
#include "core/log.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "harness.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "reference_renderer.h"

namespace harness {

using namespace gpu;

namespace {

constexpr uint32_t kQuadVertices = 6;  // as the D3D12 backend: up to a quad with no depth test may be a full screen pass

struct TextureUse {
    TextureDesc d;
    uint64_t uses = 0;
    std::string mips;  // per level past 0: mean channel difference from the level above shrunk
};

std::set<uint64_t> g_shaders;
std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_targets;  // data addresses drawn to: the surface's Format and Size
std::map<uint64_t, TextureUse> g_textures;
uint64_t g_written_uses = 0;

std::filesystem::path Dir() {
    static std::filesystem::path d = [] {
        auto p = app::ExeDir() / "cap" / "hlsl";
        std::filesystem::create_directories(p);
        return p;
    }();
    return d;
}

void WriteShader(const char* kind, uint64_t key, const std::string& src) {
    if (!g_shaders.insert(key ^ (kind[0] == 'p' ? 1 : 0)).second) return;
    char name[48];
    std::snprintf(name, sizeof name, "%s_%016llX.hlsl", kind, (unsigned long long)key);
    if (FILE* f = std::fopen((Dir() / name).string().c_str(), "w")) {
        std::fwrite(src.data(), 1, src.size(), f);
        std::fclose(f);
    }
}

// mean absolute channel difference between level l and level l - 1 shrunk 2x2
std::string MipDistances(const TextureDesc& d) {
    std::string out;
    if (d.linear || d.levels < 2) return out;
    std::vector<float> up, lo;
    uint32_t uw, uh, lw, lh;
    if (!reference::DecodeRgba(d, 0, 0, up, uw, uh)) return out;
    for (uint32_t l = 1; l < d.levels; l++) {
        if (!reference::DecodeRgba(d, l, 0, lo, lw, lh)) break;
        double sum = 0;
        for (uint32_t y = 0; y < lh; y++)
            for (uint32_t x = 0; x < lw; x++)
                for (int c = 0; c < 4; c++) {
                    float a = 0;
                    for (uint32_t k = 0; k < 4; k++) {
                        uint32_t sx = std::min(x * 2 + (k & 1), uw - 1), sy = std::min(y * 2 + (k >> 1), uh - 1);
                        a += up[(size_t(sy) * uw + sx) * 4 + c];
                    }
                    sum += std::fabs(a / 4 - lo[(size_t(y) * lw + x) * 4 + c]);
                }
        char b[16];
        std::snprintf(b, sizeof b, " %.3f", sum / (double(lw) * lh * 4));
        out += b;
        up.swap(lo);
        uw = lw;
        uh = lh;
    }
    return out;
}


// one element as the input assembler reads it, then finished as the vertex shader's VsInputs does; false out of range
bool Fetch(const Assembled& a, int r, uint32_t id, float o[4]) {
    const VertexElement& e = a.elements[size_t(r)];
    const VertexBuffer& b = a.buffers[e.buffer];
    static const struct { uint8_t f, n, kind, bytes; } kFormats[] = {
        // kind: 0 float, 1 unorm8, 2 snorm16, 3 sint16, 4 uint32, 5 bgra8
        {DXGI_FORMAT_R32_FLOAT, 1, 0, 4}, {DXGI_FORMAT_R32G32_FLOAT, 2, 0, 4}, {DXGI_FORMAT_R32G32B32_FLOAT, 3, 0, 4},
        {DXGI_FORMAT_R32G32B32A32_FLOAT, 4, 0, 4}, {DXGI_FORMAT_R8_UNORM, 1, 1, 1}, {DXGI_FORMAT_R8G8_UNORM, 2, 1, 1},
        {DXGI_FORMAT_R8G8B8A8_UNORM, 4, 1, 1}, {DXGI_FORMAT_R16_SNORM, 1, 2, 2}, {DXGI_FORMAT_R16G16_SNORM, 2, 2, 2},
        {DXGI_FORMAT_R16G16B16A16_SNORM, 4, 2, 2}, {DXGI_FORMAT_R16_SINT, 1, 3, 2}, {DXGI_FORMAT_R16G16_SINT, 2, 3, 2},
        {DXGI_FORMAT_R16G16B16A16_SINT, 4, 3, 2}, {DXGI_FORMAT_R32_UINT, 1, 4, 4}, {DXGI_FORMAT_B8G8R8A8_UNORM, 4, 5, 1},
    };
    o[0] = o[1] = o[2] = 0;
    o[3] = 1;
    for (const auto& f : kFormats) {
        if (f.f != e.format) continue;
        size_t at = size_t(id) * b.stride + e.offset;
        if (at + size_t(f.n) * f.bytes > b.bytes) {
            o[3] = 0;  // D3D12 reads an element past the buffer as zeros
            return false;
        }
        const uint8_t* p = b.src + at;
        uint32_t u = 0;
        for (int c = 0; c < f.n; c++) {
            switch (f.kind) {
                case 0: std::memcpy(&o[c], p + c * 4, 4); break;
                case 1: o[c] = p[c] / 255.0f; break;
                case 2: { int16_t v; std::memcpy(&v, p + c * 2, 2); o[c] = std::max(v / 32767.0f, -1.0f); break; }
                case 3: { int16_t v; std::memcpy(&v, p + c * 2, 2); o[c] = v; break; }
                case 4: std::memcpy(&u, p, 4); break;
                case 5: o[c] = p[c == 3 ? 3 : 2 - c] / 255.0f; break;
            }
        }
        switch (uint32_t(a.decode >> (r * 4)) & 15) {
            case kDecodeW1: case kDecodeIntW1: o[3] = 1; break;
            case kDecodePacked: {
                int32_t x = int32_t(u << 21) >> 21, y = int32_t(u << 10) >> 21, z = int32_t(u) >> 22;
                o[0] = std::max(x / 1023.0f, -1.0f), o[1] = std::max(y / 1023.0f, -1.0f), o[2] = std::max(z / 511.0f, -1.0f), o[3] = 1;
                break;
            }
            case kDecodeFloat2H: o[3] = o[2], o[2] = 0; break;
            default: break;
        }
        return true;
    }
    o[3] = 0;
    return false;
}

// the vertex ids each primitive corner reads, strips taken apart as ListIndices takes them apart
std::vector<uint32_t> Corners(const Assembled& a) {
    std::vector<uint32_t> ids;
    uint32_t n = a.index_count ? a.index_count : a.draw_count;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = i;
        if (a.index_count) {
            if (a.index16) {
                uint16_t x;
                std::memcpy(&x, static_cast<const uint8_t*>(a.index_src) + i * 2, 2);
                v = x;
            } else {
                std::memcpy(&v, static_cast<const uint8_t*>(a.index_src) + i * 4, 4);
            }
            v = uint32_t(int32_t(v) + a.base_vertex);
        }
        ids.push_back(v);
    }
    if (!a.strip) return ids;
    std::vector<uint32_t> list;
    if (a.topology == Topology::Lines) {
        for (size_t i = 0; i + 1 < ids.size(); i++) list.insert(list.end(), {ids[i], ids[i + 1]});
    } else {
        for (size_t i = 0; i + 2 < ids.size(); i++) {
            if (i & 1) list.insert(list.end(), {ids[i + 1], ids[i], ids[i + 2]});
            else list.insert(list.end(), {ids[i], ids[i + 1], ids[i + 2]});
        }
    }
    return list;
}

struct VertexAudit {
    uint64_t draws = 0, raw = 0, bad = 0, out_of_range = 0;
    std::map<uint32_t, uint64_t> types;  // D3DVSDT read raw: draws
};
VertexAudit g_vertices;

bool Same(float x, float y) { return (x != x && y != y) || std::fabs(x - y) <= 1e-6f * std::max(1.0f, std::fabs(x)); }

// raw against converted, corner by corner
bool CompareVertices(const State& s, const Assembled& raw, const Assembled& conv) {
    g_vertices.draws++;
    if (!raw.vertices.empty()) return false;  // the raw path fell back to converting
    g_vertices.raw++;
    if (s.vs)
        for (int r = 0; r < 16; r++)
            if (raw.regs >> r & 1) g_vertices.types[s.vs->inputs[size_t(r)].type]++;
    std::vector<uint32_t> a = Corners(raw), b = Corners(conv);
    bool bad = a.size() != b.size() || raw.regs != conv.regs || raw.topology != conv.topology;
    bool oor = false;
    for (size_t i = 0; i < a.size() && !bad; i++)
        for (int r = 0; r < 16 && !bad; r++) {
            if (!(raw.regs >> r & 1)) continue;
            float x[4], y[4];
            oor |= !Fetch(raw, r, a[i], x);
            Fetch(conv, r, b[i], y);
            for (int c = 0; c < 4; c++)
                if (!Same(x[c], y[c])) {
                    bad = true;
                    const VertexElement& e = raw.elements[size_t(r)];
                    const VertexBuffer& vb = raw.buffers[e.buffer];
                    LOG_FIRST(40, "audit: vertex mismatch: draw vs %X corner %zu of %zu id %u v%d type %02X raw %g %g %g %g converted %g %g %g %g;"
                              " buffer %u of %u: %u bytes stride %u offset %u, vertices %u, indexed %u base %d",
                              s.vs_handle, i, a.size(), a[i], r, s.vs ? s.vs->inputs[size_t(r)].type : 0, x[0], x[1], x[2], x[3], y[0], y[1],
                              y[2], y[3], e.buffer, raw.buffer_count, vb.bytes, vb.stride, e.offset, raw.vertex_count, raw.index_count, raw.base_vertex);
                    break;
                }
        }
    if (a.size() != b.size() || raw.regs != conv.regs || raw.topology != conv.topology)
        LOG_FIRST(20, "audit: assembly differs: corners %zu %zu regs %X %X", a.size(), b.size(), raw.regs, conv.regs);
    if (bad && s.vs) {
        std::string t;
        for (uint32_t d : s.vs->decl) {
            char b[12];
            std::snprintf(b, sizeof b, " %08X", d);
            t += b;
        }
        t += ";";
        const VertexBuffer& vb = raw.buffers[0];
        for (uint32_t k = 0; k < 18 && k * 4 + 4 <= vb.bytes; k++) {
            uint32_t w;
            std::memcpy(&w, vb.src + k * 4, 4);
            char b[12];
            std::snprintf(b, sizeof b, " %08X", w);
            t += b;
        }
        LOG_FIRST(10, "audit: declaration%s; stream 0 stride %u, declared %u", t.c_str(), s.streams[0].stride, s.vs->stream_stride[0]);
    }
    g_vertices.bad += bad;
    g_vertices.out_of_range += oor;
    return bad;
}

// faces side by side, 24 bit
void WriteTexture(const TextureDesc& d) {
    uint32_t faces = d.cube ? 6 : 1, w = 0, h = 0;
    std::vector<std::vector<float>> px(faces);
    for (uint32_t f = 0; f < faces; f++) {
        if (reference::DecodeRgba(d, 0, f, px[f], w, h)) continue;
        if (!Format(d.format).depth) return;
        // depth as grey, 0 black to 1 white
        std::vector<uint8_t> raw;
        uint32_t pitch;
        DecodeLevel(d, 0, f, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, raw, pitch);
        w = d.linear ? d.width : d.width, h = d.height;
        px[f].resize(size_t(w) * h * 4);
        for (size_t i = 0; i < size_t(w) * h; i++) {
            float z;
            std::memcpy(&z, &raw[i * 4], 4);
            px[f][i * 4] = px[f][i * 4 + 1] = px[f][i * 4 + 2] = z;
        }
    }
    char name[64];
    std::snprintf(name, sizeof name, "tex_%08X_%05llu.tga", d.data, (unsigned long long)g_frames.load());
    FILE* out = std::fopen((app::ExeDir() / "cap" / name).string().c_str(), "wb");
    if (!out) return;
    uint32_t tw = w * faces;
    uint8_t head[18] = {0, 0, 2};
    head[12] = uint8_t(tw), head[13] = uint8_t(tw >> 8), head[14] = uint8_t(h), head[15] = uint8_t(h >> 8), head[16] = 24, head[17] = 0x20;
    std::fwrite(head, 1, 18, out);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t f = 0; f < faces; f++)
            for (uint32_t x = 0; x < w; x++) {
                const float* c = &px[f][(size_t(y) * w + x) * 4];
                uint8_t bgr[3] = {uint8_t(c[2] * 255 + 0.5f), uint8_t(c[1] * 255 + 0.5f), uint8_t(c[0] * 255 + 0.5f)};
                std::fwrite(bgr, 1, 3, out);
            }
    std::fclose(out);
}

}  // namespace

void AuditDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    static const long every = Env("AUDIT");
    if (!every || g_frames % every) return;
    // as the D3D12 backend assembles it, and as the reference renderer does
    Assembled a, conv;
    bool was = g_raw_vertices;
    g_raw_vertices = true;
    bool ok = Assemble(s, prim, start, count, indices, a);
    g_raw_vertices = false;
    bool ok_conv = Assemble(s, prim, start, count, indices, conv);
    g_raw_vertices = was;
    if (ok != ok_conv) LOG_FIRST(20, "audit: one path draws, the other not: raw %d converted %d prim %u", ok, ok_conv, prim);
    if (!ok) return;
    bool vertex_bad = ok_conv && CompareVertices(s, a, conv);

    // vertex shader, as D3D12Backend::Draw classifies and keys it
    bool prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    bool quad = count <= kQuadVertices && !s.rs[kRsZEnable] && a.topology != Topology::Points;
    std::string vs = prog ? VertexProgramHlsl(s.vs->program, a.regs, a.decode)
                          : FixedVertexHlsl(a.regs, a.pretransformed, a.decode, MakeFixedKey(s));
    if (quad) vs = QuadHlsl(std::move(vs));
    WriteShader("vs", Hash(vs.data(), vs.size()), vs);
    if (vertex_bad) LOG_FIRST(20, "audit: vertex mismatch in a draw with vs_%016llX", (unsigned long long)Hash(vs.data(), vs.size()));

    if (s.rt.data) g_targets[mem::VirtualFromPhysical(s.rt.data)] = {s.rt.format, s.rt.size};
    if (s.zs.data && s.rs[kRsZEnable]) g_targets[mem::VirtualFromPhysical(s.zs.data)] = {s.zs.format, s.zs.size};
    // stage textures as the backend describes them to the pixel shader
    TexInfo info[4];
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) continue;
        const FormatInfo& fi = Format(d.format);
        info[i].kind = d.cube ? kTexCube : d.depth > 1 ? kTex3D : kTex2D;
        info[i].rect = d.linear;
        if (fi.depth) info[i].shadow = d.format == 0x2C || d.format == 0x30 ? 2 : 1;
        uint64_t key = uint64_t(d.data) << 32 ^ uint64_t(d.format) << 24 ^ uint64_t(d.levels) << 20 ^ d.width << 8 ^ d.height;
        static const uint32_t dump = uint32_t(std::strtoul(std::getenv("AUDIT_TEX") ? std::getenv("AUDIT_TEX") : "0", nullptr, 16));
        static uint64_t dumped = ~0ull;
        if (dump && d.data == dump && g_frames / 300 != dumped) {
            dumped = g_frames / 300;
            WriteTexture(d);
        }
        TextureUse& u = g_textures[key];
        if (!u.uses++) {
            u.d = d;
            u.mips = MipDistances(d);
        }
    }
    PixelKey pk = MakePixelKey(s, info);
    pk.quad = quad;
    // as the D3D12 backend: W-buffered unless XBE_WBUFFER=0
    static const bool wbuffer = !(std::getenv("XBE_WBUFFER") && *std::getenv("XBE_WBUFFER") == '0');
    pk.wbuf = wbuffer && s.rs[kRsZEnable] == 2 ? 1 : 0;
    std::string ps = PixelHlsl(pk);
    WriteShader("ps", Hash(ps.data(), ps.size()), ps);
    // a stage reading a target the game drew as a surface narrower than 32 bits
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) continue;
        auto it = g_targets.find(d.data);
        if (it == g_targets.end() || Format((it->second.first >> 8) & 0xFF).bits >= 32 || Format(d.format).depth) continue;
        static std::set<uint64_t> seen;
        if (!seen.insert(Hash(ps.data(), ps.size(), d.data + i)).second) continue;
        LOGI("audit: stage %d samples the %u bit target %08X as %s, ps_%016llX vs_%016llX", i,
                  Format((it->second.first >> 8) & 0xFF).bits, d.data, Format(d.format).name,
                  (unsigned long long)Hash(ps.data(), ps.size()), (unsigned long long)Hash(vs.data(), vs.size()));
    }
}

void AuditPresent() {
    if (!Env("AUDIT")) return;
    uint64_t uses = 0;
    for (auto& [k, u] : g_textures) uses += u.uses;
    if (uses == g_written_uses || g_frames % 60) return;
    g_written_uses = uses;
    if (FILE* v = std::fopen((app::ExeDir() / "cap" / "vertices.txt").string().c_str(), "w")) {
        std::fprintf(v, "draws %llu raw %llu mismatched %llu reading past a buffer %llu\n# D3DVSDT read raw: draws\n",
                     (unsigned long long)g_vertices.draws, (unsigned long long)g_vertices.raw, (unsigned long long)g_vertices.bad,
                     (unsigned long long)g_vertices.out_of_range);
        for (auto& [t, n] : g_vertices.types) std::fprintf(v, "%02X %llu\n", t, (unsigned long long)n);
        std::fclose(v);
    }
    FILE* f = std::fopen((app::ExeDir() / "cap" / "textures.txt").string().c_str(), "w");
    if (!f) return;
    std::fprintf(f, "# data format name width height depth levels cube linear uses : mip distances [drawn to at +offset]\n");
    for (auto& [k, u] : g_textures) {
        const TextureDesc& d = u.d;
        std::string drawn;
        for (auto it = g_targets.lower_bound(d.data); it != g_targets.end() && it->first < d.data + std::max(TotalSize(d), 1u); ++it) {
            char b[24];
            std::snprintf(b, sizeof b, " +%X", it->first - d.data);
            drawn += b;
        }
        if (!drawn.empty()) drawn = " drawn to at" + drawn;
        std::fprintf(f, "%08X %02X %s %u %u %u %u %d %d %llu :%s\n", d.data, d.format, Format(d.format).name, d.width, d.height,
                     d.depth, d.levels, d.cube, d.linear, (unsigned long long)u.uses, (u.mips + drawn).c_str());
    }
    std::fprintf(f, "# targets: data Format Size\n");
    for (auto& [data, fs] : g_targets) std::fprintf(f, "target %08X %08X %08X\n", data, fs.first, fs.second);
    std::fclose(f);
}

}  // namespace harness
