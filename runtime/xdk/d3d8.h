#pragma once
#include <mutex>

#include "gpu/state.h"

namespace d3d {

gpu::State& State();
std::recursive_mutex& Lock();
// a line from the title's code into the XBE_FRAMELOG file while one is being written
void FrameLogNote(const char* text);
// the next frame shown is blended: this much of its image over the last image, and whether the image is a new one
void SetFrameBlend(float weight, bool new_image);

}  // namespace d3d
