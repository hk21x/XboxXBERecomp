#!/bin/bash
# build.sh: the headless harness exe of one image, on Linux with clang
#   -r repo (holding sdk/ and src/)   -g generated folder of the image   -o out folder
#   -n exe name (default: the generated folder's name)
#   -x regex of sources to leave out (repeatable)   -e extra source file (repeatable)
#   -G folder for the generated objects (default <out>/gen), shared between builds of the same generated code
# env: CXX (clang++), JOBS (nproc), HOST_FLAGS (more flags for the host sources: a game's release define)
# Leaves out the window, SDL input, XAudio2 and D3D12 parts of the runtime and the sources of the repo's other images
# (src/<name> for each other folder beside the generated one); the harness supplies their stand ins.
set -e
EXCL=(); EXTRA=()
while getopts r:g:o:n:x:e:G: opt; do
    case $opt in
        r) REPO=$OPTARG ;; g) GEN=$OPTARG ;; o) OUT=$OPTARG ;; n) EXE=$OPTARG ;;
        x) EXCL+=("$OPTARG") ;; e) EXTRA+=("$(realpath "$OPTARG")") ;; G) GEN_OBJ=$OPTARG ;;
        *) sed -n 2,8p "$0"; exit 2 ;;
    esac
done
[ -n "$REPO" ] && [ -n "$GEN" ] && [ -n "$OUT" ] || { sed -n 2,8p "$0"; exit 2; }
REPO=$(realpath "$REPO"); GEN=$(realpath "$GEN"); mkdir -p "$OUT"; OUT=$(realpath "$OUT")
EXE=${EXE:-$(basename "$GEN")}; GEN_OBJ=${GEN_OBJ:-$OUT/gen}; mkdir -p "$GEN_OBJ" "$OUT/host"
H=$(dirname "$(realpath "$0")"); RT=$REPO/sdk/runtime
CXX=${CXX:-clang++}; JOBS=${JOBS:-$(nproc)}
# arm64: no fused multiply add, which the x86-64-v2 build cannot emit either (the guest's float results)
ARCH_FLAGS=-march=x86-64-v2; [ "$(uname -m)" = arm64 ] && ARCH_FLAGS=-ffp-contract=off
BASE="-std=c++20 $ARCH_FLAGS -fno-strict-aliasing"
HOST="$BASE -O1 -g -I$REPO/src -I$RT -I$GEN -I$H -I$H/compat -DXBE_RAW_VERTICES=${RAW:-0} -DXBE_RENDER_THREAD=${RT_THREAD:-0} ${HOST_FLAGS:-}"

# generated code, an object rebuilt only when its partition is newer
export CXX BASE RT GEN GEN_OBJ GEN_OPT
ls "$GEN"/x86_recomp.*.cpp "$GEN"/x86_func_mapping.cpp | xargs -P "$JOBS" -n 1 sh -c '
    o="$GEN_OBJ/$(basename "$1" .cpp).o"; [ "$o" -nt "$1" ] && exit 0
    $CXX $BASE ${GEN_OPT:--O1} -g0 -w -I"$RT" -I"$GEN" -c "$1" -o "$o.tmp" && mv "$o.tmp" "$o"' _

others=$(ls "$GEN/.." | grep -vx "$(basename "$GEN")" | paste -sd'|' -)
SRC=$(ls "$RT"/*/*.cpp | grep -Ev "/runtime/app/|/core/input.cpp|/core/wheel.cpp|/gpu/d3d12.cpp|/gpu/d3d11.cpp|/gpu/d3d_common.cpp|/audio/xaudio2.cpp|/audio/sdl_audio.cpp|/core/wheel_none.cpp")
SRC="$SRC $(ls "$REPO"/src/*/*.cpp | grep -Ev "/src/(${others:-^$})/" || true)"
SRC="$SRC $(ls "$H"/*.cpp) ${EXTRA[*]}"
for x in "${EXCL[@]}"; do SRC=$(echo $SRC | tr ' ' '\n' | grep -Ev -- "$x"); done

# host objects, keyed by the flags; rebuilt when the source or any header of the repo, harness or image is newer
stamp=$(echo "$HOST" | { md5sum 2>/dev/null || md5 -q; } | cut -c1-8)
newest=$(ls -t $(find "$RT" "$REPO/src" "$H" "$GEN/image_symbols.h" -name '*.h') | head -1)
objs=()
for f in $SRC; do
    o=$OUT/host/$(echo "${f#/}" | tr / _ | sed "s/\.cpp$/.$stamp.o/"); objs+=("$o")
    [ "$o" -nt "$f" ] && [ "$o" -nt "$newest" ] && continue
    echo "compile $f"
    rm -f "$o"
    $CXX $HOST -c "$f" -o "$o" &
    [ "$(jobs -r | wc -l)" -ge "$JOBS" ] && wait -n
done
wait
$CXX -o "$OUT/$EXE" "${objs[@]}" "$GEN_OBJ"/*.o -lpthread
echo "built $OUT/$EXE"
