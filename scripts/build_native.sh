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
# The repository. The engine is $ROOT; modules, the studio, the synth and
# the rest are beside it, here.
REPO="$(cd "$ROOT/.." && pwd)"
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
  "$ROOT/core/src/sim/sectormotion.cpp"
  "$ROOT/core/src/sim/weapon.cpp"
  "$ROOT/core/src/sim/crush.cpp"
  "$ROOT/core/src/geom/map.cpp"
  "$ROOT/core/src/geom/triangulate.cpp"
  "$ROOT/core/src/geom/mapstore.cpp"
  "$ROOT/core/src/geom/mapfile.cpp"
  "$ROOT/core/src/geom/rail.cpp"
  "$ROOT/core/src/physics/collide.cpp"
  "$ROOT/core/src/physics/controller.cpp"
  "$ROOT/core/src/nav/navgraph.cpp"
)

# `build_native.sh <arch> mapc`: the map compiler alone (what the packager's
# kit build needs), without the test binaries -- each of those compiles the
# whole core again, and all of them take a long while.
if [[ "${2:-}" == "mapc" ]]; then
  clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/tools/mapc/main.cpp" -o "$OUT/mapc"
  echo "[build] mapc -> $OUT/mapc"
  exit 0
fi

echo "[build] arch=$ARCH -> $OUT"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_math.cpp"    -o "$OUT/test_math"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_proto.cpp"   -o "$OUT/test_proto"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_physics.cpp" -o "$OUT/test_physics"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_abilities.cpp" -o "$OUT/test_abilities"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_triangulate.cpp" -o "$OUT/test_triangulate"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_mapfile.cpp"     -o "$OUT/test_mapfile"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_sectormotion.cpp" -o "$OUT/test_sectormotion"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_crush.cpp"        -o "$OUT/test_crush"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_interact.cpp"     -o "$OUT/test_interact"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_props.cpp"        -o "$OUT/test_props"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_bodies.cpp"       -o "$OUT/test_bodies"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_sweep.cpp"        -o "$OUT/test_sweep"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_sight.cpp"        -o "$OUT/test_sight"
# The weapon machine both sides run (docs/WEAPON_PREDICTION.md).
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_weapons.cpp"      -o "$OUT/test_weapons"
# Several maps in one store, joined at their open edges, added and removed live.
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_chunks.cpp"       -o "$OUT/test_chunks"
# The metamap: where each level sits, and what that means has to be resident.
#
# Built HERE rather than beside the Lua tests because it needs neither LuaJIT nor
# a world -- the parse takes text and the policy takes numbers -- so it runs in
# the same dependency-free stage the determinism gate does.
clang++ "${FLAGS[@]}" -I"$ROOT/server/src" \
        "$ROOT/server/src/world/metamap.cpp" "$ROOT/server/tests/test_metamap.cpp" \
        -o "$OUT/test_metamap"
# What a client holds across its cell boundary, and the server's input buffer:
# both pure enough to need no Lua and no level.
clang++ "${FLAGS[@]}" -I"$ROOT/server/src" "$ROOT/server/tests/test_neighbours.cpp" \
        -o "$OUT/test_neighbours"
clang++ "${FLAGS[@]}" -I"$ROOT/server/src" "${SRCS[@]}" "$ROOT/server/tests/test_input_ring.cpp" \
        -o "$OUT/test_input_ring"
# A fixture generator, not a test: it prints wire bytes for the JS decoder.
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tools/ui_fixture.cpp"        -o "$OUT/ui_fixture"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_turning.cpp"      -o "$OUT/test_turning"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_nav.cpp"          -o "$OUT/test_nav"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_rail.cpp"         -o "$OUT/test_rail"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/test_path.cpp"         -o "$OUT/test_path"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/tools/mapc/main.cpp"             -o "$OUT/mapc"
clang++ "${FLAGS[@]}" "${SRCS[@]}" "$ROOT/core/tests/golden_trace.cpp" -o "$OUT/golden_trace"
echo "[build] ok"
