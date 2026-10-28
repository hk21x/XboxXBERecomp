#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "gpu/state.h"

namespace gpu {

// What the renderer uploads a console texture format as
enum class Host : uint8_t { BGRA8, BC1, BC2, BC3, R32F, R16Unorm, None };

struct FormatInfo {
    const char* name;
    uint8_t bits;      // per texel, 4 or 8 bytes per block for the DXT formats
    bool swizzled;     // Morton order in memory
    bool compressed;   // 4x4 blocks
    bool depth;
    Host host;
};

const FormatInfo& Format(uint32_t xbox_format);

// a D3DPixelContainer decoded
struct TextureDesc {
    uint32_t data = 0;  // guest virtual address of level 0
    uint32_t format = 0;
    uint32_t width = 1, height = 1, depth = 1, levels = 1;
    uint32_t pitch = 0;  // linear formats only
    bool cube = false;
    bool linear = false;
};

bool Describe(uint32_t header, TextureDesc& out);
bool DescribeSurface(const Surface& s, TextureDesc& out);
uint32_t LevelSize(const TextureDesc& d, uint32_t level);
uint32_t LevelOffset(const TextureDesc& d, uint32_t level);
uint32_t FaceSize(const TextureDesc& d);  // cube face stride
uint32_t TotalSize(const TextureDesc& d);

// converts one level (one face) from guest memory into what Host describes, rows tightly packed
void DecodeLevel(const TextureDesc& d, uint32_t level, uint32_t face, const uint8_t* guest, const uint32_t* palette,
                 std::vector<uint8_t>& out, uint32_t& row_pitch);

uint32_t LinearColorFormat(uint32_t fmt);
uint32_t LinearDepthFormat(uint32_t fmt);
void SurfaceSize(const Surface& s, uint32_t& w, uint32_t& h);

// CopyRects done in guest memory, false when the formats do not allow it
bool CopySurfaceMemory(const Surface& src, const Surface& dst, uint32_t count, uint32_t rects, uint32_t points);

// the title locked a resource, the next bind re-uploads it
void MarkDirty(uint32_t resource);
uint64_t DirtyEpoch(uint32_t data_va);

// A title's own picture for a texture (GameInfo hooks, e.g. a sharper redraw of a HUD atlas): the hardware renderers
// sample it instead of the guest bytes at data, whatever its size, for a swizzled or compressed texture (a linear
// one's coordinates are in its own texels), while those guest_bytes stay as they were when it was set (the memory
// given to something else drops it). B8G8R8A8 rows, width x height; version is new for each SetReplacement.
struct Replacement {
    uint32_t data = 0, guest_bytes = 0;
    uint64_t guest_hash = 0;
    uint32_t width = 0, height = 0;
    uint64_t version = 0;
    bool sharpened = false;  // made by Sharpen2D (Settings::sharp_2d), not the title
    // the texture it stands for (Sharpen2D's; 0 any): the same memory read in another format or size is not it
    uint32_t format = 0, src_width = 0, src_height = 0;
    std::vector<uint32_t> bgra;
};
void SetReplacement(uint32_t data_va, uint32_t guest_bytes, uint32_t width, uint32_t height, std::vector<uint32_t> bgra);
// a texture a 2D draw samples (GameInfo::sharpen_2d), from the render thread: its level 0 copied and enlarged on a
// background thread, then set as its replacement (sharpened); one whose bytes keep changing is left alone
void Sharpen2D(const TextureDesc& d);
// a picture enlarged: alpha that is mostly fully in or out sharpened as a mask (UpscaleMask), else interpolated;
// colour Catmull-Rom interpolated weighted by alpha. B8G8R8A8 in and out.
std::vector<uint32_t> UpscalePicture(const std::vector<uint32_t>& bgra, uint32_t width, uint32_t height, uint32_t scale);
// one level as B8G8R8A8 rows (the DXT formats decoded too); false for formats it cannot
bool DecodeBgra(const TextureDesc& d, uint32_t level, const uint8_t* guest, std::vector<uint32_t>& out);
void ClearReplacement(uint32_t data_va);
// false (and dropped) when the guest bytes changed since it was set; a renderer checks once a frame
bool ReplacementHolds(const Replacement& r);
std::shared_ptr<const Replacement> FindReplacement(uint32_t data_va);  // null when there is none
bool ReplacementFits(const Replacement& r, const TextureDesc& d);  // read as the texture it was made for
// a mask (text, outlines: alpha values, mostly 0 or full) enlarged `scale` times with sharp edges: Catmull-Rom
// interpolated, then thresholded at half with an edge about one output pixel wide (from the interpolated gradient)
std::vector<uint8_t> UpscaleMask(const uint8_t* alpha, uint32_t width, uint32_t height, uint32_t scale);
// its mip chain down to 1x1, each level box filtered from the one above (level 0 the picture)
std::vector<std::vector<uint32_t>> ReplacementLevels(const Replacement& r);

// vertex declarations and pixel shader constant routing, both follow the XDK's definitions
void ParseDeclaration(VertexShader& vs);
// SetPixelShaderConstant: the stages of the current shader mapped to these registers take the values
void ApplyPixelConstants(State& s, uint32_t first, uint32_t count);
uint32_t PackColor(const std::array<float, 4>& c);
// the texel index of (x, y) in a swizzled (Morton order) w x h surface
uint32_t SwizzledOffset(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

}  // namespace gpu
