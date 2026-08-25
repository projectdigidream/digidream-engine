#!/usr/bin/env bash
#
# Builds the core test binaries with plain clang++ for one architecture.
#
# This exists so determinism can be verified before CMake/Emscripten are
# installed, and so CI has a dependency-free fallback. The real build is
# CMakeLists.txt; both must pass the SAME compiler flags, which is why the
# flag list lives in one place here and is mirrored in cmake/CoreFlags.cmake.
#
set -euo pipefail

ARCH="${1:-arm64}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build/$ARCH"
mkdir -p "$OUT"

# -ffp-contract=off and -fno-fast-math are belt-and-braces: core/ contains no
# floating point at all, but a stray float in future code must not silently
# acquire fused-multiply-add on one target and not another.
FLAGS=(
  -std=c++17
  -O2
  -g
  -Wall -Wextra -Wpedantic
  -Wno-unused-function
  -Werror
  -fno-fast-math
  -ffp-contract=off
  -fno-strict-aliasing
  -I"$ROOT/core/include"
)

if [[ "$ARCH" != "native" ]]; then
  FLAGS+=(-arch "$ARCH")
fi

SRCS=(
  "$ROOT/core/src/math/trig.cpp"
  "$ROOT/core/src/proto/messages.cpp"
  "$ROOT/core/src/sim/world.cpp"
  "$ROOT/core/src/sim/testmap.cpp"
  "$ROOT/core/src/geom/map.cpp"
  "$ROOT/core/src/physics/collide.cpp"
  "$ROOT/core/src/physics/controller.cpp"
)

echo "[build] arch=$ARCH -> $OUT"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_math.cpp"    -o "$OUT/test_math"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_proto.cpp"   -o "$OUT/test_proto"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_physics.cpp" -o "$OUT/test_physics"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_abilities.cpp" -o "$OUT/test_abilities"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/golden_trace.cpp" -o "$OUT/golden_trace"
echo "[build] ok"
