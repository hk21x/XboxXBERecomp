#pragma once
// The few SSE2 operations the runtime uses, on x86 from the compiler's headers and on arm64 in NEON.
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <emmintrin.h>
#else
#include <arm_neon.h>
#include <cstdint>

typedef int64x2_t __m128i;

inline __m128i _mm_setzero_si128() { return vdupq_n_s64(0); }
inline __m128i _mm_loadu_si128(const __m128i* p) { return vreinterpretq_s64_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(p))); }
inline void _mm_store_si128(__m128i* p, __m128i v) { vst1q_u8(reinterpret_cast<uint8_t*>(p), vreinterpretq_u8_s64(v)); }
inline __m128i _mm_or_si128(__m128i a, __m128i b) { return vorrq_s64(a, b); }
inline __m128i _mm_xor_si128(__m128i a, __m128i b) { return veorq_s64(a, b); }
inline __m128i _mm_set1_epi16(int16_t v) { return vreinterpretq_s64_s16(vdupq_n_s16(v)); }
inline __m128i _mm_min_epi16(__m128i a, __m128i b) {
    return vreinterpretq_s64_s16(vminq_s16(vreinterpretq_s16_s64(a), vreinterpretq_s16_s64(b)));
}
inline __m128i _mm_max_epi16(__m128i a, __m128i b) {
    return vreinterpretq_s64_s16(vmaxq_s16(vreinterpretq_s16_s64(a), vreinterpretq_s16_s64(b)));
}
inline __m128i _mm_cmpeq_epi8(__m128i a, __m128i b) {
    return vreinterpretq_s64_u8(vceqq_u8(vreinterpretq_u8_s64(a), vreinterpretq_u8_s64(b)));
}
// bit i: the top bit of byte i
inline int _mm_movemask_epi8(__m128i v) {
    static const int8_t shifts[16] = {0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7};
    uint8x16_t top = vshrq_n_u8(vreinterpretq_u8_s64(v), 7);
    uint8x16_t bits = vshlq_u8(top, vld1q_s8(shifts));
    return vaddv_u8(vget_low_u8(bits)) | (vaddv_u8(vget_high_u8(bits)) << 8);
}
#endif
