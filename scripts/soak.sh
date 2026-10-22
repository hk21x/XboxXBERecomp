#!/usr/bin/env bash
# Soak test: races back to back, headless, for N minutes (default 60).
#   scripts/soak.sh [minutes]
# The game's SOAK_PAD program (scripts/game.env) prints the pad script for that many seconds into .local/soak.txt
# (PGR2's: Instant Action, 60 seconds of throttle, Quit, again). Every 30 seconds the game process's memory is
# sampled into .local/soak-mem.txt.
# Reports: races started (cities from the log), traps, whether frames kept coming to the end, memory growth.
set -uo pipefail
. "$(dirname "$0")/common.sh"
MIN=${1:-60}
SECS=$((MIN * 60))
PAD=.local/soak.txt
LOG=.local/run/1/logs/${GAME_LOG}_001.log
MEM=.local/soak-mem.txt

[ -n "$SOAK_PAD" ] || { echo "set SOAK_PAD in scripts/game.env (a program printing the pad script for N seconds)"; exit 1; }
"$SOAK_PAD" "$SECS" > $PAD || exit 1

rm -f $LOG $MEM
"$SDK_SCRIPTS/harness.sh" "$SECS" "$PAD" > .local/soak.out 2>&1 &
H=$!
for i in $(seq 120); do pgrep -x game > /dev/null && break; timeout 1 tail -f /dev/null; done
start=$(date +%s)
while kill -0 $H 2> /dev/null; do
  pid=$(pgrep -x game | head -1)
  if [ -n "$pid" ]; then
    f=$(grep -o "renderer: frame [0-9]*" $LOG 2> /dev/null | tail -1 | awk '{print $3}')
    echo "$(( $(date +%s) - start )) ${f:-0} $(awk '/VmRSS|VmSize|VmData/{printf "%s ", $2}' /proc/$pid/status 2> /dev/null)" >> $MEM
  fi
  timeout 30 tail -f /dev/null
done

echo "soak: $MIN minutes"
races=$(grep -c "^# race" $PAD)
cities=$(grep -o "Welcome to [A-Za-z. ]*\|city [A-Za-z]*" $LOG | sort | uniq -c | tr '\n' ' ')
echo "races scripted: $races"
traps=$(grep -cE "guest trap|is not implemented|trapping stub|unknown code" $LOG)
echo "traps: $traps"
last=$(grep "renderer: frame" $LOG | tail -1)
echo "last frame line: $last"
t=$(echo "$last" | awk '{ t = $2; sub(/\]/, "", t); print int(t) }')
[ "${t:-0}" -ge $((SECS - 30)) ] && echo "frames kept coming to the end" || echo "FRAMES STOPPED at ${t}s of ${SECS}s"
grep -E "\[(error)\]" $LOG | head -5
# races driven: windows of 60 frames drawing more than 300 draws a frame, counted where they begin
grep "renderer: frame" $LOG | awk '{ match($0, /frame [0-9]+/); f = substr($0, RSTART + 6, RLENGTH - 6) + 0
    match($0, /[0-9]+ draws/); d = substr($0, RSTART, RLENGTH - 6) + 0
    if (f > pf) { per = (d - pd) / (f - pf); if (per > 300 && !on) races++; on = per > 300 } pf = f; pd = d }
    END { print "3D scenes (races, and the attract demo before the title):", races + 0 }'
# memory, columns: seconds frame VmSize VmRSS VmData (kB); the first sample after 5 minutes against the last
awk '$1 >= 300 && !a { a = $4; ad = $5; at = $1 } { b = $4; bd = $5; bt = $1 } END {
  if (a && bt > at) printf "RSS %.0f MB at %ds -> %.0f MB at %ds (%+.1f MB an hour), data %.0f -> %.0f MB\n",
    a / 1024, at, b / 1024, bt, (b - a) / 1024 / ((bt - at) / 3600), ad / 1024, bd / 1024 }' $MEM
