#!/bin/sh
# decompile from a Ghidra project: decomp.sh PROJECT PROGRAM OUT args...
P=$1; G=$2; O=$3; shift 3
rm -f "$O"
/opt/ghidra/support/analyzeHeadless "$(cd "$(dirname "$0")/.." && pwd)/re/ghidra" "$P" -process "$G" -noanalysis -readOnly \
  -scriptPath "$(cd "$(dirname "$0")" && pwd)/ghidra" -postScript Decomp.java "$O" "$@" > "$O.log" 2>&1
[ -f "$O" ] || tail -20 "$O.log"
