#pragma once
// Render state (D3DRS) and texture stage state (D3DTSS) numbers of an XDK generation. The runtime keeps 5659's
// (gpu::kRs*, gpu::kTss*) and translates where it reads or writes the title's own tables; layout.toml's
// StateNumbering picks the generation, 5659 when absent. Simple render states need no table: they are keyed by the
// method the title's D3D_SimpleRenderStateEncode gives each one.
#include <array>
#include <cstdint>

#include "gpu/state.h"
#include "image_symbols.h"

#ifndef XDK_StateNumbering
#define XDK_StateNumbering 5659
#endif
static_assert(XDK_StateNumbering == 5659 || XDK_StateNumbering == 3948 || XDK_StateNumbering == 4039,
              "layout.toml StateNumbering: 5659, 4039 or 3948 (a new generation needs its table in xdk/state_numbering.h)");

namespace xdk::states {
constexpr uint32_t kNone = 0xFFFFFFFFu;

struct Numbering {
    uint32_t deferred_first;                   // the title's first deferred render state, lower ones are simple
    uint32_t count;                            // the title's render state count
    std::array<uint32_t, gpu::kRsMax> rs;      // 5659's index -> the title's, kNone where the title has no such state
    std::array<uint32_t, gpu::kTssCount> tss;  // the same for texture stage states
};

constexpr Numbering Make5659() {
    Numbering n{gpu::kRsDeferredFirst, gpu::kRsMax, {}, {}};
    for (uint32_t i = 0; i < gpu::kRsMax; i++) n.rs[i] = i;
    for (uint32_t i = 0; i < gpu::kTssCount; i++) n.tss[i] = i;
    return n;
}

// 3948 (the launch XDK): 82 simple, 34 deferred and 28 complex states. Deferred are 5659's FogEnable to
// PatchSegments, 10 lower (no SwapFilter, PresentationInterval or unused slots). Complex lack 5659's
// MultiSampleMode, MultiSampleRenderTargetMode (3948's MultiSampleType at 134 sits between) and SampleAlpha.
// Texture stage states put the combiner states (ColorOp to TextureTransformFlags) first, then AddressU to AlphaKill.
constexpr Numbering Make3948() {
    Numbering n{82, 144, {}, {}};
    for (auto& r : n.rs) r = kNone;
    for (uint32_t i = gpu::kRsFogEnable; i <= gpu::kRsPointSizeMax + 2; i++) n.rs[i] = i - 10;
    for (uint32_t i = gpu::kRsPsTextureModes; i <= gpu::kRsMultiSampleMask; i++) n.rs[i] = i - 20;
    n.rs[gpu::kRsShadowFunc] = 135;
    n.rs[gpu::kRsLineWidth] = 136;
    for (uint32_t i = gpu::kRsDxt1NoiseEnable; i <= gpu::kRsDoNotCullUncompressed; i++) n.rs[i] = i - 22;
    for (uint32_t i = 0; i < gpu::kTssCount; i++) n.tss[i] = i;
    for (uint32_t i = gpu::kTssAddressU; i <= gpu::kTssAlphaKill; i++) n.tss[i] = i + 10;
    for (uint32_t i = gpu::kTssColorOp; i <= gpu::kTssTextureTransformFlags; i++) n.tss[i] = i - 12;
    return n;
}

// 4039: 82 simple, 35 deferred and 29 complex states. Deferred as 3948's, with SwapFilter last (the frame buffer init
// sets it); complex are 5659's less SampleAlpha. Texture stage states as 5659's.
constexpr Numbering Make4039() {
    Numbering n{82, 146, {}, {}};
    for (auto& r : n.rs) r = kNone;
    for (uint32_t i = gpu::kRsFogEnable; i <= gpu::kRsPointSizeMax + 2; i++) n.rs[i] = i - 10;
    n.rs[gpu::kRsPresentationInterval - 1] = 116;
    for (uint32_t i = gpu::kRsPsTextureModes; i <= gpu::kRsLineWidth; i++) n.rs[i] = i - 19;
    for (uint32_t i = gpu::kRsDxt1NoiseEnable; i <= gpu::kRsDoNotCullUncompressed; i++) n.rs[i] = i - 20;
    for (uint32_t i = 0; i < gpu::kTssCount; i++) n.tss[i] = i;
    return n;
}

constexpr Numbering kNumbering = XDK_StateNumbering == 3948   ? Make3948()
                                 : XDK_StateNumbering == 4039 ? Make4039()
                                                              : Make5659();

// the title's render state -> 5659's, kNone for a simple state or one 5659 lacks
constexpr uint32_t RsFromTitle(uint32_t t) {
    for (uint32_t i = gpu::kRsDeferredFirst; i < gpu::kRsMax; i++)
        if (kNumbering.rs[i] == t) return i;
    return kNone;
}
constexpr uint32_t TssFromTitle(uint32_t t) {
    for (uint32_t i = 0; i < gpu::kTssCount; i++)
        if (kNumbering.tss[i] == t) return i;
    return kNone;
}
}  // namespace xdk::states
