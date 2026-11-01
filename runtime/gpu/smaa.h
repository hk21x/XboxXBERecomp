#pragma once
#include <cstdint>

namespace gpu {

// SMAA on the finished frame (smaa.cpp): entry points vsedge/psedge (t0 the frame), vsweight/psweight (t0 edges, t1 area,
// t2 search), vsblend/psblend (t0 the frame, t1 weights); b1 uvrect (unused), then 1/w, 1/h, w, h
extern const char* const kSmaaHlsl;
extern const unsigned char* const kSmaaArea;    // R8G8, kSmaaAreaWidth x kSmaaAreaHeight
extern const unsigned char* const kSmaaSearch;  // R8, kSmaaSearchWidth x kSmaaSearchHeight
constexpr uint32_t kSmaaAreaWidth = 160, kSmaaAreaHeight = 560, kSmaaSearchWidth = 64, kSmaaSearchHeight = 16;

}  // namespace gpu
