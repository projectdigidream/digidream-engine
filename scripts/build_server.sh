#!/usr/bin/env bash
#
# Builds the authoritative server. POSIX sockets and poll(2), so macOS and Linux.
#
# server/ may use wall clock, sockets and the heap -- none of that touches the
# simulation. core/ stays integer-only and ambient-input-free; the tick loop here
# only decides WHEN a tick happens in real time, never what it computes.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build/server"
mkdir -p "$OUT"

clang++ -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror \
  -I"$ROOT/core/include" -I"$ROOT/server/src" \
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
  "$ROOT/core/src/physics/collide.cpp" \
  "$ROOT/core/src/physics/controller.cpp" \
  "$ROOT/server/src/net/link.cpp" \
  "$ROOT/server/src/main.cpp" \
  -o "$OUT/dnp-server"

echo "[build] $OUT/dnp-server"
