#!/usr/bin/env bash
# Smoke test: boot headless, play SMOKE_PAD (scripts/game.env: title -> a race, then throttle) and fail unless
#   - the harness builds and runs, with no trapping stub, unimplemented kernel export or guest trap logged,
#   - a 2D menu is up just before the menu choice (SMOKE_MENU in scripts/game.env: under 100 draws a frame),
#   - the race draws: the last 300 frames average more than 100 draws a frame, and frames are still presented in
#     the last 20 seconds.
# With SMOKE_NEXT_GEN (scripts/game.env), for a title whose play is in a second xbe it launches (XLaunchNewImage):
# the first run must relaunch into it (no traps, the menu check as above), then that xbe runs on the first run's hard
# disk and launch data with SMOKE_NEXT_PAD for SMOKE_NEXT_SECS, and the drawing checks apply to that run.
# Run before every fast-forward of main.   scripts/smoke.sh [seconds, default 240]
# KEEP_HDD=1 reuses the last run's hard disk (a normal boot); by default it is a first boot (format, cache copy).
set -uo pipefail
. "$(dirname "$0")/common.sh"
SECS=${1:-$SMOKE_SECS}
PAD=${PAD:-$SMOKE_PAD}  # another pad script checks that the test fails without the race
LOG=.local/run/1/logs/${GAME_LOG}_001.log
fail() { echo "SMOKE FAIL: $*"; exit 1; }

"$SDK_SCRIPTS/harness.sh" "$SECS" "$PAD" > .local/smoke.out 2>&1 || fail "harness did not build or run (.local/smoke.out)"
[ -f "$LOG" ] || fail "no log at $LOG"

no_traps() {
  local traps
  traps=$(grep -E 'guest trap|is not implemented|trapping stub|unknown code' "$1" | head -5)
  [ -z "$traps" ] || fail "traps:"$'\n'"$traps"
}
no_traps "$LOG"

# draws a frame between vertical blanks a and b, from the log's every-60-frames lines
window() { grep 'renderer: frame' "$LOG" | awk -v a="$1" -v b="$2" '{
    match($0, /frame [0-9]+/); f = substr($0, RSTART + 6, RLENGTH - 6) + 0
    match($0, /[0-9]+ draws/); d = substr($0, RSTART, RLENGTH - 6) + 0
    match($0, /vblank [0-9]+/); v = substr($0, RSTART + 7, RLENGTH - 7) + 0
    if (v >= a && v <= b) { if (!n++) { f0 = f; d0 = d } f1 = f; d1 = d } }
    END { print (f1 > f0) ? int((d1 - d0) / (f1 - f0)) : -1 }'; }
# the menu just before the choice draws little; a 3D scene would mean the presses
# missed it
menu=none
if [ -n "$SMOKE_MENU" ]; then
  menu=$(window $SMOKE_MENU)
  [ "$menu" -ge 0 ] && [ "$menu" -lt 100 ] || fail "not on the menu before the choice: $menu draws a frame (vertical blanks $SMOKE_MENU)"
fi

# the game drawing in the log $1 of a run of $2 seconds: more than 100 draws a frame over its last 300 frames, and
# frames still presented in its last 20 seconds (a crash after the race started fails too)
drawing() {
  local frames draws last
  read -r frames draws < <(grep 'renderer: frame' "$1" | tail -6 | awk '{
      match($0, /frame [0-9]+/); f = substr($0, RSTART + 6, RLENGTH - 6) + 0
      match($0, /[0-9]+ draws/); d = substr($0, RSTART, RLENGTH - 6) + 0
      if (NR == 1) { f0 = f; d0 = d } f1 = f; d1 = d } END { print f1 - f0, d1 - d0 }')
  [ "${frames:-0}" -gt 0 ] || fail "no frames presented"
  last=$(grep 'renderer: frame' "$1" | tail -1 | awk '{ t = $2; sub(/\]/, "", t); print int(t) }')
  [ "${last:-0}" -ge $(( $2 - 20 )) ] || fail "frames stopped at ${last}s of ${2}s"
  per=$(( draws / frames ))
  [ "$per" -gt 100 ] || fail "race not drawing: $per draws a frame over the last $frames frames"
  span=$frames
}

if [ -n "$SMOKE_NEXT_GEN" ]; then
  grep -q 'relaunching into' "$LOG" || fail "no relaunch into the next xbe ($SMOKE_NEXT_NAME)"
  bash sdk/harness/build.sh -r . -g "$SMOKE_NEXT_GEN" -o .local/harness_next -n "$SMOKE_NEXT_NAME" > .local/smoke_next.out 2>&1 ||
    fail "the next xbe's harness did not build (.local/smoke_next.out)"
  rm -rf .local/run_next && mkdir -p .local/run_next/1 && cp -r .local/run/1/hdd .local/run_next/1/ ||
    fail "no hard disk to carry over"
  bash sdk/harness/run.sh -x ".local/harness_next/$SMOKE_NEXT_NAME" -g "${GAME:-assets}" -o .local/run_next \
    -t "$SMOKE_NEXT_SECS" -s "$SMOKE_NEXT_PAD" >> .local/smoke_next.out 2>&1
  NEXT_LOG=.local/run_next/1/logs/${GAME_LOG}_001.log
  [ -f "$NEXT_LOG" ] || fail "no log at $NEXT_LOG"
  grep -q 'launch data from the previous run' "$NEXT_LOG" || fail "the next xbe did not get the launch data"
  no_traps "$NEXT_LOG"
  drawing "$NEXT_LOG" "$SMOKE_NEXT_SECS"
  echo "SMOKE OK: menu $menu draws a frame, relaunched into $SMOKE_NEXT_NAME, which draws $per a frame over the last $span frames, no traps"
  exit 0
fi

drawing "$LOG" "$SECS"
echo "SMOKE OK: menu $menu draws a frame, race $per draws a frame over the last $span frames, no traps"
