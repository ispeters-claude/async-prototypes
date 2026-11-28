#!/bin/sh
# Nanoseconds per call of every scenario, by configuration: the best of seven
# runs of N calls (bench time). On a shared machine, read the ratios.
#
#   ./time.sh [CXX [N]]
#   CONFIGS="3:0:0 3:2:0" ./time.sh clang++ 1000000
set -eu
CXX=${1:-g++}
N=${2:-1000000}
DIR=$(cd "$(dirname "$0")" && pwd)
STDEXEC=${STDEXEC:-$(ls -d "$DIR"/../build/*/_deps/stdexec_30acdb0-src/include | head -1)}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
CONFIGS=${CONFIGS:-"3:0:0 3:1:0 3:2:0 3:3:0 3:0:1 3:2:1"}
for c in $CONFIGS; do
  m=${c%%:*}; rest=${c#*:}; d=${rest%%:*}; a=${rest#*:}
  "$CXX" -std=c++23 -O2 -DNDEBUG -DTH_MODE=$m -DSLICE_DEST=$d -DSLICE_ALLOC=$a \
    -isystem "$STDEXEC" -I"$DIR" "$DIR/bench.cpp" "$DIR/stores.cpp" -o "$OUT/bench"
  echo "== $c"
  "$OUT/bench" time "$N"
done
