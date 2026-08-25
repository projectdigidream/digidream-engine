#!/usr/bin/env bash
#
# Builds the server-side Lua tests.
#
# Separate from scripts/build_native.sh on purpose: that one compiles the core
# tests against core sources ONLY, which is what lets the determinism gate run
# before any third-party toolchain is installed. A Lua test in there would make
# LuaJIT a prerequisite for checking determinism, and those two things must stay
# independent.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build/lua"
LJ="$ROOT/third_party/luajit"

if [[ ! -f "$LJ/src/libluajit.a" ]]; then
  echo "[lua-tests] LuaJIT not installed -- run scripts/setup_luajit.sh" >&2
  exit 2
fi

mkdir -p "$OUT"

FLAGS=(-std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror
       -I"$ROOT/core/include" -I"$ROOT/server/src" -I"$LJ/src")
LINK=("$LJ/src/libluajit.a" -lm)
[[ "$(uname -s)" == "Darwin" ]] || LINK+=(-ldl)

SRCS=("$ROOT/server/src/lua/vm.cpp" "$ROOT/server/src/lua/fx_binding.cpp")

clang++ "${FLAGS[@]}" "${SRCS[@]}" \
        "$ROOT/server/tests/test_lua_vm.cpp" "${LINK[@]}" -o "$OUT/test_lua_vm"
clang++ "${FLAGS[@]}" "${SRCS[@]}" \
        "$ROOT/server/tests/test_lua_fx.cpp" "${LINK[@]}" -o "$OUT/test_lua_fx"
echo "[lua-tests] $OUT/test_lua_vm $OUT/test_lua_fx"
