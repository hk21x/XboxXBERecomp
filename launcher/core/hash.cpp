#include "core/hash.h"

#include <cstring>
#include <fstream>
#include <vector>

namespace launcher {

namespace {

uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
uint32_t Ror(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

std::string HexOf(const uint32_t* words, int n) {
    std::string s;
    for (int i = 0; i < n; i++)
        for (int b = 28; b >= 0; b -= 4) s += "0123456789abcdef"[(words[i] >> b) & 15];
    return s;
}

// the common Merkle-Damgard padding: 0x80, zeros, the length in bits big endian
template <typename H>
void Pad(H& h, uint64_t len) {
    uint8_t tail[72] = {0x80};
    size_t fill = (len % 64 < 56 ? 56 : 120) - len % 64;
    uint64_t bits = len * 8;
    for (int i = 0; i < 8; i++) tail[fill + i] = uint8_t(bits >> (56 - 8 * i));
    h.Update(tail, fill + 8);
}

constexpr uint32_t kK256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

}  // namespace

Sha1::Sha1() : h_{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0} {}

void Sha1::Block(const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = Be32(p + 4 * i);
    for (int i = 16; i < 80; i++) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
        else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
        else f = b ^ c ^ d, k = 0xCA62C1D6;
        uint32_t t = Rol(a, 5) + f + e + k + w[i];
        e = d, d = c, c = Rol(b, 30), b = a, a = t;
    }
    h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e;
}

void Sha1::Update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len) {
        size_t at = len_ % 64, n = std::min<size_t>(64 - at, len);
        std::memcpy(buf_ + at, p, n);
        len_ += n, p += n, len -= n;
        if (len_ % 64 == 0) Block(buf_);
    }
}

std::string Sha1::Hex() {
    Pad(*this, len_);
    return HexOf(h_, 5);
}

Sha256::Sha256()
    : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::Block(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = Be32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25)) + ((e & f) ^ (~e & g)) + kK256[i] + w[i];
        uint32_t t2 = (Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
}

void Sha256::Update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len) {
        size_t at = len_ % 64, n = std::min<size_t>(64 - at, len);
        std::memcpy(buf_ + at, p, n);
        len_ += n, p += n, len -= n;
        if (len_ % 64 == 0) Block(buf_);
    }
}

std::string Sha256::Hex() {
    Pad(*this, len_);
    return HexOf(h_, 8);
}

std::string FileSha256(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    Sha256 h;
    std::vector<char> buf(1 << 20);
    while (in) {
        in.read(buf.data(), std::streamsize(buf.size()));
        h.Update(buf.data(), size_t(in.gcount()));
    }
    return h.Hex();
}

}  // namespace launcher
