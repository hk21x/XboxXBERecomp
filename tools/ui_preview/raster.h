#pragma once
// ImDrawData drawn by a small software rasteriser, and a PNG writer: the settings window's and the launcher's previews
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

struct Image {
    int w, h;
    std::vector<uint8_t> rgb;
};
static void Triangle(Image& im, const ImDrawVert* v[3], const ImVec4& clip, const uint8_t* font, int fw, int fh) {
    float area = (v[1]->pos.x - v[0]->pos.x) * (v[2]->pos.y - v[0]->pos.y) - (v[2]->pos.x - v[0]->pos.x) * (v[1]->pos.y - v[0]->pos.y);
    if (area == 0) return;
    float x0 = std::max({std::min({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x}), clip.x, 0.0f});
    float x1 = std::min({std::max({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x}), clip.z, float(im.w)});
    float y0 = std::max({std::min({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y}), clip.y, 0.0f});
    float y1 = std::min({std::max({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y}), clip.w, float(im.h)});
    for (int y = int(y0); y < std::ceil(y1); y++)
        for (int x = int(x0); x < std::ceil(x1); x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float b[3];
            b[0] = ((v[1]->pos.x - px) * (v[2]->pos.y - py) - (v[2]->pos.x - px) * (v[1]->pos.y - py)) / area;
            b[1] = ((v[2]->pos.x - px) * (v[0]->pos.y - py) - (v[0]->pos.x - px) * (v[2]->pos.y - py)) / area;
            b[2] = 1 - b[0] - b[1];
            if (b[0] < 0 || b[1] < 0 || b[2] < 0) continue;
            float c[4] = {}, u = 0, t = 0;
            for (int i = 0; i < 3; i++) {
                ImU32 col = v[i]->col;
                for (int k = 0; k < 4; k++) c[k] += b[i] * float((col >> (k * 8)) & 255);
                u += b[i] * v[i]->uv.x, t += b[i] * v[i]->uv.y;
            }
            // bilinear, as the renderers sample the atlas
            float fx = u * fw - 0.5f, fy = t * fh - 0.5f;
            int ix = std::clamp(int(std::floor(fx)), 0, fw - 1), iy = std::clamp(int(std::floor(fy)), 0, fh - 1);
            int jx = std::min(ix + 1, fw - 1), jy = std::min(iy + 1, fh - 1);
            float ax = fx - std::floor(fx), ay = fy - std::floor(fy);
            float tex = ((font[iy * fw + ix] * (1 - ax) + font[iy * fw + jx] * ax) * (1 - ay) + (font[jy * fw + ix] * (1 - ax) + font[jy * fw + jx] * ax) * ay) / 255;
            float a = c[3] / 255 * tex;
            uint8_t* p = &im.rgb[size_t(y * im.w + x) * 3];
            for (int k = 0; k < 3; k++) p[k] = uint8_t(std::clamp(p[k] * (1 - a) + c[k] * a, 0.0f, 255.0f));
        }
}
static void Draw(Image& im) {
    // something behind the window, as a game frame would be
    for (int y = 0; y < im.h; y++)
        for (int x = 0; x < im.w; x++) {
            uint8_t* p = &im.rgb[size_t(y * im.w + x) * 3];
            p[0] = uint8_t(20 + 40 * y / im.h), p[1] = uint8_t(30 + 50 * y / im.h), p[2] = uint8_t(70 + 90 * y / im.h);
        }
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* font;
    int fw, fh;
    io.Fonts->GetTexDataAsAlpha8(&font, &fw, &fh);
    ImDrawData* dd = ImGui::GetDrawData();
    for (ImDrawList* l : dd->CmdLists)
        for (const ImDrawCmd& c : l->CmdBuffer)
            for (unsigned i = 0; i + 2 < c.ElemCount; i += 3) {
                const ImDrawVert* v[3];
                for (int k = 0; k < 3; k++) v[k] = &l->VtxBuffer[l->IdxBuffer[c.IdxOffset + i + k] + c.VtxOffset];
                Triangle(im, v, c.ClipRect, font, fw, fh);
            }
}
static void Png(const Image& im, const std::string& path) {
    std::vector<uint8_t> raw;
    for (int y = 0; y < im.h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), &im.rgb[size_t(y * im.w) * 3], &im.rgb[size_t(y * im.w) * 3] + im.w * 3);
    }
    std::vector<uint8_t> z = {0x78, 1};
    uint32_t s1 = 1, s2 = 0;
    for (uint8_t b : raw) s1 = (s1 + b) % 65521, s2 = (s2 + s1) % 65521;
    for (size_t off = 0; off < raw.size();) {
        size_t n = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + n == raw.size());
        z.push_back(n & 255), z.push_back(n >> 8), z.push_back(~n & 255), z.push_back((~n >> 8) & 255);
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
    }
    for (int i = 3; i >= 0; i--) z.push_back((((s2 << 16) | s1) >> (i * 8)) & 255);
    std::ofstream f(path, std::ios::binary);
    f.write("\x89PNG\r\n\x1a\n", 8);
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        auto be = [&](uint32_t v) { char b[4] = {char(v >> 24), char(v >> 16), char(v >> 8), char(v)}; f.write(b, 4); };
        be(uint32_t(data.size()));
        std::vector<uint8_t> all(type, type + 4);
        all.insert(all.end(), data.begin(), data.end());
        uint32_t crc = ~0u;
        for (uint8_t b : all) {
            crc ^= b;
            for (int k = 0; k < 8; k++) crc = crc >> 1 ^ (0xEDB88320u & -(crc & 1));
        }
        f.write(reinterpret_cast<const char*>(all.data()), std::streamsize(all.size()));
        be(~crc);
    };
    auto u = [](uint32_t v) { return std::vector<uint8_t>{uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; };
    std::vector<uint8_t> ihdr = u(uint32_t(im.w));
    for (uint8_t b : u(uint32_t(im.h))) ihdr.push_back(b);
    for (uint8_t b : {8, 2, 0, 0, 0}) ihdr.push_back(b);
    chunk("IHDR", ihdr), chunk("IDAT", z), chunk("IEND", {});
}

