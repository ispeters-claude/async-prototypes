#!/bin/sh
# Build time, code size and instructions per run of build_time.cpp's chain,
# by mode, depth and how often erase_handles cuts it.
#
#   ./build_time.sh [CXX]
#   DEPTHS="16 32" FIREWALLS="0 4" MODES="2 3" ./build_time.sh g++
set -eu
CXX=${1:-g++}
DIR=$(cd "$(dirname "$0")" && pwd)
STDEXEC=${STDEXEC:-$(ls -d "$DIR"/../build/*/_deps/stdexec_30acdb0-src/include | head -1)}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

totals() {  # Ir Bi from a Callgrind output file
  awk '/^events:/ { for (i = 2; i <= NF; i++) col[$i] = i - 1 }
       /^totals:/ { print $(col["Ir"] + 1), $(col["Bi"] + 1) }' "$1"
}

printf '%-8s %5s %8s %8s %8s %10s %6s\n' mode depth firewall build_s text_B Ir/run Bi/run
for m in ${MODES:-2 3}; do
  mode=$(echo EAGER ERASED TYPED VARIANT | cut -d' ' -f$((m + 1)))
  for depth in ${DEPTHS:-8 16 32 64}; do
    for fw in ${FIREWALLS:-0 8 4}; do
      start=$(date +%s.%N)
      "$CXX" -std=c++23 -O2 -DNDEBUG -static -DTH_MODE=$m -DDEPTH=$depth -DFIREWALL=$fw \
        -isystem "$STDEXEC" -I"$DIR" "$DIR/build_time.cpp" -o "$OUT/bt"
      end=$(date +%s.%N)
      size=$(nm -S --size-sort -C "$OUT/bt" | awk '$3 ~ /[tTwW]/ && /run_chain|th::/ { s += strtonum("0x" $2) } END { print s + 0 }')
      for k in 1 3; do
        if [ $k = 1 ]; then arg=; else arg=x; fi
        valgrind --tool=callgrind --branch-sim=yes \
          --callgrind-out-file="$OUT/cg.$k" "$OUT/bt" $arg >/dev/null 2>&1
      done
      set -- $(totals "$OUT/cg.1") $(totals "$OUT/cg.3")
      awk -v m="$mode" -v d="$depth" -v f="$fw" -v s="$start" -v e="$end" -v z="$size" \
          -v a="$1" -v b="$2" -v c="$3" -v g="$4" \
        'BEGIN { printf "%-8s %5d %8d %8.2f %8d %10.1f %6.2f\n", m, d, f, e - s, z, (c - a) / 4000, (g - b) / 4000 }'
    done
  done
done
