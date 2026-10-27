#!/usr/bin/env bash
# Runs one of sdk/scripts/ghidra/*.java on the analysed project (read-only) and prints its output lines.
#   scripts/ghidra.sh Decomp 13db90 13cca0        decompile functions, with their callers
#   scripts/ghidra.sh Disasm 14b380 40             instructions from an address
#   scripts/ghidra.sh FindOperand 0x27ecc8 "[0x291a24]"   instructions whose text has each argument
#   scripts/ghidra.sh Callers 1c42f0               callers of a function
#   (also DumpRange, PushBufferScan, DeviceRefs)
# Needs the project made once (CLAUDE.md, Ghidra): .local/game.exe imported into ~/ghidra-projects/<GHIDRA_PROJECT>.
# A game with more xbes imports each under its own name; GHIDRA_PROGRAM=driving.exe picks another (default game.exe).
set -euo pipefail
. "$(dirname "$0")/common.sh"
script=$1; shift
timeout 900 /opt/ghidra/support/analyzeHeadless "$HOME/ghidra-projects" "${GHIDRA_PROJECT:?set GHIDRA_PROJECT in scripts/game.env}" -process "${GHIDRA_PROGRAM:-game.exe}" -noanalysis -readOnly \
  -scriptPath "$SDK_SCRIPTS/ghidra" -postScript "$script.java" "$@" 2>&1 |
  grep -v -E '^(INFO|WARN|ERROR) [^>]*$|\(HeadlessAnalyzer\) *$|^(openjdk|OpenJDK)|^    /(opt|home)/.*ghidra_scripts *$|^    /.*scripts/ghidra *$' |
  sed 's/ (GhidraScript) *$//; s/^INFO  [A-Za-z]*\.java> //'
