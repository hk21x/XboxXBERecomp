#!/usr/bin/env bash
# Build tools/filter_preview from the final pass's HLSL as it stands. usage: build.sh <out dir>
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); out=$1; mkdir -p "$out"
awk '/^float luma\(float3 c\)/{p=1} /^float4 psfinal\(O i\)/{p=0} p' "$here/../../runtime/gpu/d3d_common.cpp" > "$out/final_pass.inc"
clang++ -std=c++20 -O2 -w -I"$out" "$here/main.cpp" -o "$out/filter_preview"
clang++ -std=c++20 -O2 -w -I"$here/../../runtime" "$here/upscale_test.cpp" "$here/../../runtime/gpu/upscale.cpp" -o "$out/upscale_test"
