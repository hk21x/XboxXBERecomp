#!/usr/bin/env bash
# Build and run tools/ui_preview. usage: build.sh <imgui source dir> <out dir> [width height]; UI_WINDOWS=<dir holding Fonts/tahoma.ttf>
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); im=$1; out=$2; shift 2
mkdir -p "$out"
clang++ -std=c++20 -O1 -g -w -I"$here/stub" -I"$here/../../runtime" -I"$im" "$here/main.cpp" "$here/../../runtime/app/overlay.cpp" \
  "$im/imgui.cpp" "$im/imgui_draw.cpp" "$im/imgui_tables.cpp" "$im/imgui_widgets.cpp" -o "$out/ui_preview"
"$out/ui_preview" "$out" "$@"
