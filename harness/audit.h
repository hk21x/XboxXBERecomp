#pragma once
#include <cstdint>

#include "gpu/state.h"

// AUDIT=N: every Nth frame, the shaders and textures a GPU backend would be given, see audit.cpp
namespace harness {

void AuditDraw(const gpu::State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices);
void AuditPresent();  // rewrites cap/textures.txt every 60 frames when it changed

}  // namespace harness
