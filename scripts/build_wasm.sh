#!/usr/bin/env bash
#
# Builds the simulation core to WebAssembly for the client.
#
# The flags mirror scripts/build_native.sh on purpose. This binary and the
# server's must compute identical results, so any optimisation or math flag that
# applies to one has to apply to the other -- that is what check_determinism.sh
# verifies, and this is where a discrepancy would be introduced.
#
set -euo pipefail

if ! command -v em++ >/dev/null 2>&1; then
  echo "em++ not found -- run scripts/setup_emsdk.sh then: source emsdk/emsdk_env.sh" >&2
  exit 1
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/client/wasm/dist"
mkdir -p "$OUT"

em++ -std=c++17 -O2 \
  -Wall -Wextra -Wpedantic -Werror \
  -fno-fast-math -ffp-contract=off \
  -I"$ROOT/core/include" \
  "$ROOT/core/src/math/trig.cpp" \
  "$ROOT/core/src/proto/messages.cpp" \
  "$ROOT/core/src/sim/world.cpp" \
  "$ROOT/core/src/sim/testmap.cpp" \
  "$ROOT/core/src/sim/sectormotion.cpp" \
  "$ROOT/core/src/sim/crush.cpp" \
  "$ROOT/core/src/geom/map.cpp" \
  "$ROOT/core/src/geom/triangulate.cpp" \
  "$ROOT/core/src/geom/mapstore.cpp" \
  "$ROOT/core/src/geom/mapfile.cpp" \
  "$ROOT/core/src/geom/rail.cpp" \
  "$ROOT/core/src/physics/collide.cpp" \
  "$ROOT/core/src/physics/controller.cpp" \
  "$ROOT/core/src/nav/navgraph.cpp" \
  "$ROOT/client/wasm/bindings.cpp" \
  -sMODULARIZE=1 \
  -sEXPORT_ES6=1 \
  -sENVIRONMENT=node,web \
  -sALLOW_MEMORY_GROWTH=1 \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAP8,HEAPU8,HEAP32,HEAPU32 \
  -sEXPORTED_FUNCTIONS=_malloc,_free \
  -o "$OUT/dnp_core.mjs"

echo "[build] $OUT/dnp_core.mjs"
