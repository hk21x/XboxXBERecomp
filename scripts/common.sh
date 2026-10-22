# Sourced by the shared scripts: the game repository's root (the folder holding sdk/) becomes the working folder,
# and the game's own values come from its scripts/game.env (defaults below).
SDK_SCRIPTS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$SDK_SCRIPTS/../.."
GAME_XBE=default.xbe          # the xbe in assets/ the build recompiles
GAME_LOG=game                 # the log's prefix (GameInfo::log): .local/run/1/logs/<GAME_LOG>_001.log headless
GAME_TARGET=                  # the add_xbe_image target (recompile_<GAME_TARGET>)
GHIDRA_PROJECT=               # the Ghidra project under ~/ghidra-projects
SMOKE_PAD=scripts/pad/race.txt  # smoke.sh: title -> a race, then throttle
SMOKE_MENU=                   # smoke.sh: "first last" vertical blanks where a 2D menu is up (under 100 draws a frame)
SOAK_PAD=                     # soak.sh: a program printing a pad script for N seconds of races back to back
LINK_HOST=scripts/pad/link_host.txt LINK_JOIN=scripts/pad/link_join.txt
LINK_RACE='area loads'        # linktest.sh: a log line each race load writes (the second one is the race)
[ -f scripts/game.env ] && . scripts/game.env
