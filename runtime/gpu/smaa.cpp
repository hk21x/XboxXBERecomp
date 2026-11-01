// SMAA 1x (Jimenez et al., runtime/gpu/smaa) on the finished frame: its three passes and lookup textures for both renderers
#include "gpu/d3d_common.h"

#include "gpu/smaa/AreaTex.h"
#include "gpu/smaa/SearchTex.h"

namespace gpu {

const char* const kSmaaHlsl =
R"(#define SMAA_CUSTOM_SL 1
#define SMAA_PRESET_HIGH 1
cbuffer B : register(b1) { float4 uvrect; float4 smaa_rt; };
#define SMAA_RT_METRICS smaa_rt
Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
Texture2D tex2 : register(t2);
SamplerState smaa_linear : register(s0);
SamplerState smaa_point : register(s1);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(smaa_linear, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(smaa_point, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(smaa_linear, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(smaa_linear, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(smaa_point, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(smaa_linear, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
)"
#include "gpu/smaa/SMAA.hlsl.inc"
R"(
struct SmaaV { float4 p : SV_Position; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1; float4 o0 : TEXCOORD2; float4 o1 : TEXCOORD3; float4 o2 : TEXCOORD4; };
SmaaV SmaaBase(uint id) {
    SmaaV o = (SmaaV)0;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
SmaaV vsedge(uint id : SV_VertexID) {
    SmaaV o = SmaaBase(id);
    float4 f[3];
    SMAAEdgeDetectionVS(o.uv, f);
    o.o0 = f[0], o.o1 = f[1], o.o2 = f[2];
    return o;
}
SmaaV vsweight(uint id : SV_VertexID) {
    SmaaV o = SmaaBase(id);
    float4 f[3];
    SMAABlendingWeightCalculationVS(o.uv, o.pix, f);
    o.o0 = f[0], o.o1 = f[1], o.o2 = f[2];
    return o;
}
SmaaV vsblend(uint id : SV_VertexID) {
    SmaaV o = SmaaBase(id);
    SMAANeighborhoodBlendingVS(o.uv, o.o0);
    return o;
}
float4 psedge(SmaaV i) : SV_Target {
    float4 f[3] = {i.o0, i.o1, i.o2};
    return float4(SMAALumaEdgeDetectionPS(i.uv, f, tex0), 0, 0);
}
float4 psweight(SmaaV i) : SV_Target {
    float4 f[3] = {i.o0, i.o1, i.o2};
    return SMAABlendingWeightCalculationPS(i.uv, i.pix, f, tex0, tex1, tex2, float4(0, 0, 0, 0));
}
float4 psblend(SmaaV i) : SV_Target { return SMAANeighborhoodBlendingPS(i.uv, i.o0, tex0, tex1); }
)";

const unsigned char* const kSmaaArea = areaTexBytes;
const unsigned char* const kSmaaSearch = searchTexBytes;

}  // namespace gpu
