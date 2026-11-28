#!/bin/sh
# Instructions (Ir) and indirect branches (Bi) per call of each scenario,
# under Callgrind: the difference between N and 3N calls, over 2N.
#
#   ./cg.sh [CXX [N]]          e.g. ./cg.sh g++ 1000
#   CONFIGS="3:0:0 2:1:0" ./cg.sh    only these TH_MODE:SLICE_DEST:SLICE_ALLOC
#   SCENARIOS="get_hit get_miss" ./cg.sh    only these
#
# The synchronous scenarios don't depend on the configuration and run in
# the first one only. STDEXEC is stdexec's include directory at 30acdb0;
# by default, the copy a CMake preset fetched under build/.
set -eu
CXX=${1:-g++}
N=${2:-1000}
DIR=$(cd "$(dirname "$0")" && pwd)
STDEXEC=${STDEXEC:-$(ls -d "$DIR"/../build/*/_deps/stdexec_30acdb0-src/include | head -1)}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
# VARIANT in every destination design, allocating at connect and at start;
# TYPED in every destination design, allocating at connect.
CONFIGS=${CONFIGS:-"3:0:0 3:1:0 3:2:0 3:3:0 3:0:1 3:1:1 3:2:1 3:3:1 2:0:0 2:1:0 2:2:0 2:3:0"}

totals() {  # Ir Bi from a Callgrind output file
  awk '/^events:/ { for (i = 2; i <= NF; i++) col[$i] = i - 1 }
       /^totals:/ { print $(col["Ir"] + 1), $(col["Bi"] + 1) }' "$1"
}

printf '%-8s %-7s %-8s %-26s %10s %6s\n' mode dest alloc scenario Ir/call Bi/call
first=1
for c in $CONFIGS; do
  m=${c%%:*}; rest=${c#*:}; d=${rest%%:*}; a=${rest#*:}
  mode=$(echo EAGER ERASED TYPED VARIANT | cut -d' ' -f$((m + 1)))
  dest=$(echo CHAIN ONCE RECORD OUT | cut -d' ' -f$((d + 1)))
  alloc=$(echo connect start | cut -d' ' -f$((a + 1)))
  # The callees in their own translation unit, without LTO.
  "$CXX" -std=c++23 -O2 -DNDEBUG -static -DTH_MODE=$m -DSLICE_DEST=$d -DSLICE_ALLOC=$a \
    -isystem "$STDEXEC" -I"$DIR" "$DIR/bench.cpp" "$DIR/stores.cpp" -o "$OUT/bench"
  for s in ${SCENARIOS:-$("$OUT/bench" list)}; do
    case $s in sync_*) [ $first = 1 ] || continue ;; esac
    for k in 1 3; do
      valgrind --tool=callgrind --branch-sim=yes \
        --callgrind-out-file="$OUT/cg.$k" "$OUT/bench" "$s" $((k * N)) \
        >/dev/null 2>&1
    done
    set -- $(totals "$OUT/cg.1") $(totals "$OUT/cg.3")
    awk -v m="$mode" -v d="$dest" -v al="$alloc" -v s="$s" -v n="$N" \
        -v a="$1" -v b="$2" -v c="$3" -v e="$4" \
      'BEGIN { printf "%-8s %-7s %-8s %-26s %10.1f %6.2f\n", m, d, al, s, (c - a) / (2 * n), (e - b) / (2 * n) }'
  done
  first=0
done
