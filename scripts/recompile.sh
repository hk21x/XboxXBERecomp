#!/usr/bin/env bash
# Regenerate generated/game from assets/<GAME_XBE> and the config
set -euo pipefail
. "$(dirname "$0")/common.sh"
cmake --build build/linux --target "recompile_${GAME_TARGET:?set GAME_TARGET in scripts/game.env}"
