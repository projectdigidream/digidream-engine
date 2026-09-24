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
# WHERE THE BINARY GOES.
#
# `build/server` by default, which is what scripts/run_stack.sh runs. The test
# suite overrides it, and the reason is not tidiness: relinking this file while
# a server is executing it REPLACES THE IMAGE UNDER A RUNNING PROCESS, and the
# process dies. From the outside that is an edge logging "upstream lost" every
# time somebody runs the tests, which looks like a networking fault and is not.
OUT="${DNP_SERVER_OUT:-$ROOT/build/server}"
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

# DNP_SERVER_SAN=1 builds with AddressSanitizer and UBSan.
#
# For catching a crash that only happens while somebody is playing. A plain
# build dies with a signal and no explanation; this one prints the exact line,
# the allocation it came from, and the stack. It is roughly 2x slower, which is
# fine for reproducing a fault and not for a session you care about.
#
#   DNP_SERVER_SAN=1 ./scripts/build_server.sh
#   ./scripts/run_stack.sh --module doom2 2>&1 | tee /tmp/dnp.log
SAN_FLAGS=""
if [[ -n "${DNP_SERVER_SAN:-}" ]]; then
  SAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
  echo "[build] AddressSanitizer + UBSan enabled (slower; for diagnosing a crash)"
fi

clang++ -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror $SAN_FLAGS \
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
  "$ROOT/server/src/mobs/mob_compile.cpp" \
  "$ROOT/server/src/mobs/mob_eval.cpp" \
  "$ROOT/server/src/mobs/mob_binding.cpp" \
  "$ROOT/server/src/main.cpp" \
  "$ROOT/third_party/luajit/src/libluajit.a" \
  -lm $LUA_EXTRA_LIBS \
  -o "$OUT/dnp-server.new"

# ATOMIC, even into the default directory. `mv` replaces the directory entry
# while a running process keeps the inode it already opened, so a server that
# is mid-game survives a rebuild -- where writing in place corrupts it.
mv -f "$OUT/dnp-server.new" "$OUT/dnp-server"

echo "[build] $OUT/dnp-server"
