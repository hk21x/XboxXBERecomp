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

// Reads the hardware commands the title has pushed so far (draws it writes into the push buffer itself are only seen
// at the next library call). A hook calls it before it changes State(), so those draws keep the state they were
// pushed under: Juiced's HUD texts are pushed so and took the next gadget's squeeze_shift now and then.
void Pending();

}  // namespace d3d
