#!/usr/bin/env bash
#
# THE determinism gate. Builds the golden trace for every target available on
# this machine and asserts all of them emit a byte-identical hash stream.
#
# Client-side prediction (design 9.3) is only correct because the browser's wasm
# build and the server's native build run the same physics and reach the same
# answer. If that stops holding, prediction degrades into permanent mispredicts
# with no gameplay cause -- and the symptom (rubber-banding under load) looks
# nothing like the cause. This script is how that is caught on the commit that
# introduces it rather than months later.
#
# Targets are skipped, not failed, when their toolchain is absent, so this runs
# usefully on a laptop and exhaustively in CI.
#
set -euo pipefail

TICKS="${TICKS:-10000}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$ROOT/build/determinism"
mkdir -p "$WORK"

FLAGS=(-std=c++17 -O2 -Wall -Wextra -Werror -fno-fast-math -ffp-contract=off -I"$ROOT/core/include")
SRCS=("$ROOT/core/src/math/trig.cpp" "$ROOT/core/src/proto/messages.cpp" "$ROOT/core/src/sim/world.cpp" "$ROOT/core/src/sim/testmap.cpp" "$ROOT/core/src/sim/sectormotion.cpp" "$ROOT/core/src/sim/crush.cpp" "$ROOT/core/src/geom/map.cpp" "$ROOT/core/src/geom/triangulate.cpp" "$ROOT/core/src/geom/mapstore.cpp" "$ROOT/core/src/geom/mapfile.cpp" "$ROOT/core/src/physics/collide.cpp" "$ROOT/core/src/physics/controller.cpp" "$ROOT/core/tests/golden_trace.cpp")

declare -a NAMES=()
declare -a TRACES=()

emit() { NAMES+=("$1"); TRACES+=("$2"); }

# --- native targets -------------------------------------------------------
for arch in arm64 x86_64; do
  if clang++ "${FLAGS[@]}" -arch "$arch" "${SRCS[@]}" -o "$WORK/gt_$arch" 2>/dev/null; then
    if "$WORK/gt_$arch" 1 >/dev/null 2>&1; then
      "$WORK/gt_$arch" "$TICKS" > "$WORK/trace_$arch.txt"
      emit "native-$arch" "$WORK/trace_$arch.txt"
    else
      echo "  skip native-$arch (builds, but cannot execute here)"
    fi
  else
    echo "  skip native-$arch (no cross-compile support)"
  fi
done

# --- wasm -----------------------------------------------------------------
# The client target. Until emsdk is installed this is skipped, which means the
# gate is only partially enforced -- installing emsdk is a Phase 1 prerequisite,
# not an optional nicety.
if command -v em++ >/dev/null 2>&1 && command -v node >/dev/null 2>&1; then
  em++ "${FLAGS[@]}" -sENVIRONMENT=node -sALLOW_MEMORY_GROWTH=1 \
       "${SRCS[@]}" -o "$WORK/gt_wasm.js" 2>/dev/null
  node "$WORK/gt_wasm.js" "$TICKS" > "$WORK/trace_wasm.txt"
  emit "wasm32" "$WORK/trace_wasm.txt"
else
  echo "  skip wasm32 (emsdk not installed -- run scripts/setup_emsdk.sh)"
fi

# --- compare --------------------------------------------------------------
COUNT=${#NAMES[@]}
if [[ "$COUNT" -lt 2 ]]; then
  echo "FAIL: only $COUNT target(s) available; determinism cannot be verified."
  exit 1
fi

REF_NAME="${NAMES[0]}"
REF_TRACE="${TRACES[0]}"
FAILED=0

for ((i = 1; i < COUNT; ++i)); do
  if diff -q "$REF_TRACE" "${TRACES[$i]}" >/dev/null; then
    echo "  ok   ${NAMES[$i]} == $REF_NAME"
  else
    FAILED=1
    # awk, not `diff --unchanged-line-format`: that flag is GNU-only and BSD
    # diff (macOS) rejects it outright.
    FIRST=$(awk 'NR==FNR { a[FNR] = $0; n = FNR; next }
                 { if (FNR > n || $0 != a[FNR]) { print $1; exit } }' \
              "$REF_TRACE" "${TRACES[$i]}")
    echo "  FAIL ${NAMES[$i]} diverges from $REF_NAME at tick ${FIRST:-?}"
    echo "       bisect with: scripts/bisect_divergence.sh $REF_TRACE ${TRACES[$i]}"
  fi
done

if [[ "$FAILED" -ne 0 ]]; then
  echo "DETERMINISM GATE FAILED"
  exit 1
fi

echo "determinism ok: $COUNT targets, $TICKS ticks, identical hash streams"
echo "  final $(tail -1 "$REF_TRACE")"
