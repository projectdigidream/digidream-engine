#!/usr/bin/env bash
#
# Builds and runs the test suites under AddressSanitizer and
# UndefinedBehaviorSanitizer.
#
# This matters most for core/proto: those decoders parse bytes supplied by
# whoever is on the other end of the socket, and their contract is that ANY byte
# string either decodes or is cleanly rejected. The fuzz pass inside
# test_proto.cpp only proves that if something is watching for out-of-bounds
# reads while it runs -- which is this.
#
# UBSan also covers the arithmetic rules the physics contract depends on: signed
# overflow is UB, and a compiler is free to resolve it differently between two
# builds, which is precisely the class of divergence the fixed-point core exists
# to eliminate.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build/san"
mkdir -p "$OUT"

FLAGS=(
  -std=c++17 -O1 -g
  -fsanitize=address,undefined
  -fno-omit-frame-pointer
  -fno-sanitize-recover=undefined   # a UB finding must fail the run, not warn
  -Wall -Wextra -Werror
  -I"$ROOT/core/include"
)

SRCS=("$ROOT/core/src/math/trig.cpp" "$ROOT/core/src/proto/messages.cpp" "$ROOT/core/src/sim/world.cpp" "$ROOT/core/src/sim/testmap.cpp" "$ROOT/core/src/sim/sectormotion.cpp" "$ROOT/core/src/geom/map.cpp" "$ROOT/core/src/geom/triangulate.cpp" "$ROOT/core/src/geom/mapstore.cpp" "$ROOT/core/src/geom/mapfile.cpp" "$ROOT/core/src/physics/collide.cpp" "$ROOT/core/src/physics/controller.cpp")

export ASAN_OPTIONS=detect_leaks=0
export UBSAN_OPTIONS=print_stacktrace=1

for t in test_math test_proto test_physics test_abilities test_triangulate test_mapfile test_sectormotion; do
  echo "[san] $t"
  clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/$t.cpp" -o "$OUT/$t"
  "$OUT/$t" | tail -1
done

echo "sanitizers ok"
