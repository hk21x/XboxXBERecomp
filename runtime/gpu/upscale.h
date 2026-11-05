#pragma once
#include <cstdint>
#include <vector>

namespace gpu {

// a picture at twice its size, B8G8R8A8 rows: new pixels are interpolated along the local edge's direction, not across
// it (directional cubic convolution), so edges stay clean where a plain enlargement stairs or blurs. wrap_u and wrap_v:
// the picture tiles that way, its far side is the neighbour at the edge
std::vector<uint32_t> UpscaleEdgeDirected2x(const std::vector<uint32_t>& bgra, uint32_t width, uint32_t height, bool wrap_u, bool wrap_v);

}  // namespace gpu
