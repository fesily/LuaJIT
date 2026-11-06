#!/bin/sh
# Hardware-counter measurement for the jit.aot benchmarks.
#
# macOS ONLY: uses Instruments' CPU Counters through `xcrun xctrace`, which
# needs a full Xcode installation and an accepted license
# (sudo xcodebuild -license accept). On other platforms use bench.lua only.
#
# Usage (from the repository root):
#   tests/aot-hw/counters.sh [case ...]        # default: int1 fp1
#   AOT_HW_DIR=/tmp/aot-hw tests/aot-hw/counters.sh
#
# For every case and every engine (interp, aot, jit) it records a trace and
# prints the median of each counter metric per 1 ms sample.
set -e
cd "$(dirname "$0")/../.."
DIR=${AOT_HW_DIR:-/tmp/aot-hw}
TEMPLATE=${TEMPLATE:-CPU Counters}
CASES=${*:-int1 fp1}
MULT=${MULT:-10}   # 1e9 iterations: long enough for stable 1 kHz samples
export LUA_PATH='./src/?.lua;;'   # bench.lua needs to require("jit.aot")

[ "$(uname -s)" = Darwin ] || { echo "counters.sh: macOS only (needs xcrun xctrace)"; exit 1; }
command -v xcrun >/dev/null 2>&1 || { echo "counters.sh: xcrun not found (install Xcode)"; exit 1; }
if xcrun xctrace version 2>&1 | grep -q "have not agreed"; then
  echo "counters.sh: accept the Xcode license first: sudo xcodebuild -license accept"
  exit 1
fi
mkdir -p "$DIR"

for c in $CASES; do
  # Warm up: build the AOT module (and the C compiler output) before tracing.
  ./src/luajit -joff tests/aot-hw/workload.lua "$c" aot "$MULT" >/dev/null
  for eng in interp aot jit; do
    if [ "$eng" = jit ]; then FLAGS=""; else FLAGS="-joff"; fi
    out="$DIR/$c-$eng.trace"
    rm -rf "$out"
    xcrun xctrace record --template "$TEMPLATE" --output "$out" --launch -- \
      ./src/luajit $FLAGS tests/aot-hw/workload.lua "$c" "$eng" "$MULT" >/dev/null 2>&1
    xcrun xctrace export --input "$out" \
      --xpath '/trace-toc/run[@number="1"]/data/table[@schema="MetricTable"]' \
      > "$out.xml" 2>/dev/null
    echo "=== $c / $eng"
    python3 tests/aot-hw/counters_parse.py "$out.xml"
  done
done
