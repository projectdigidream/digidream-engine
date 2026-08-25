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

# The ONE build script that links Lua. scripts/check_source_lists.sh asserts
# that: the wasm client, the sanitizer build and the determinism gate must never
# see it, because the client predicts by running the same core and Lua is the
# thing it must not run.
if [[ ! -f "$ROOT/third_party/luajit/src/libluajit.a" ]]; then
  echo "[build] LuaJIT missing -- run scripts/setup_luajit.sh" >&2
  exit 2
fi
LUA_EXTRA_LIBS=""
[[ "$(uname -s)" == "Darwin" ]] || LUA_EXTRA_LIBS="-ldl"

clang++ -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror \
  -I"$ROOT/core/include" -I"$ROOT/server/src" -I"$ROOT/third_party/luajit/src" \
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
  "$ROOT/server/src/net/link.cpp" \
  "$ROOT/server/src/lua/vm.cpp" \
  "$ROOT/server/src/lua/fx_binding.cpp" \
  "$ROOT/server/src/lua/world_binding.cpp" \
  "$ROOT/server/src/main.cpp" \
  "$ROOT/third_party/luajit/src/libluajit.a" \
  -lm $LUA_EXTRA_LIBS \
  -o "$OUT/dnp-server"

echo "[build] $OUT/dnp-server"
