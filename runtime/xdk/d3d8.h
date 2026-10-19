#pragma once
#include <mutex>

#include "gpu/state.h"

namespace d3d {

gpu::State& State();
std::recursive_mutex& Lock();
// a line from the title's code into the XBE_FRAMELOG file while one is being written
void FrameLogNote(const char* text);

}  // namespace d3d
