#!/usr/bin/env bash
#
# Runs everything CI runs, in the order that fails cheapest-first.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "=== 1/5  purity guards ==="
./scripts/check_no_floats.sh
# Lua belongs in the server build and nowhere else. Until Phase 4 the six core
# source lists were identical by construction and a mistake showed up as a link
# error; now they are required to differ in one specific way, which no link
# error can check for you.
./scripts/check_source_lists.sh
# Rendering attributes arrived in Phase 5. They are structurally out of
# MapData; this catches the other direction -- a physics routine handed a
# RenderData because it was convenient.
./scripts/check_render_isolation.sh

echo
echo "=== 2/5  core unit + property tests ==="
HOST_ARCH="$(uname -m)"
./scripts/build_native.sh "$HOST_ARCH" >/dev/null
"./build/$HOST_ARCH/test_math"
"./build/$HOST_ARCH/test_proto"
"./build/$HOST_ARCH/test_physics"
"./build/$HOST_ARCH/test_abilities"
"./build/$HOST_ARCH/test_triangulate"
"./build/$HOST_ARCH/test_mapfile"
"./build/$HOST_ARCH/test_sectormotion"
"./build/$HOST_ARCH/test_crush"
"./build/$HOST_ARCH/test_interact"
"./build/$HOST_ARCH/test_props"
"./build/$HOST_ARCH/test_nav"
"./build/$HOST_ARCH/test_rail"

# The map compiler must round-trip the authored map: mapc bakes it, and the
# loader re-triangulates and compares, so this also proves the compiler and the
# runtime have not drifted apart.
"./build/$HOST_ARCH/mapc" maps/test.map  "build/$HOST_ARCH/test.dnpm"
"./build/$HOST_ARCH/mapc" maps/crush.map  "build/$HOST_ARCH/crush.dnpm"
"./build/$HOST_ARCH/mapc" maps/phase3.map "build/$HOST_ARCH/phase3.dnpm"
"./build/$HOST_ARCH/mapc" maps/doors.map  "build/$HOST_ARCH/doors.dnpm"
"./build/$HOST_ARCH/mapc" maps/level_a.map "build/$HOST_ARCH/level_a.dnpm"
"./build/$HOST_ARCH/mapc" maps/level_b.map "build/$HOST_ARCH/level_b.dnpm"
# Floating platforms with open edges -- the only map here that is not a sealed
# box, and the only one where walking off an edge is a fall rather than a wall.
"./build/$HOST_ARCH/mapc" maps/void.map   "build/$HOST_ARCH/void.dnpm"

# A generated stress map: a few hundred irregular concave sectors, regenerated
# from a fixed seed so it is the same map every run. The hand-authored maps have
# a dozen sectors and most of them are boxes, which exercises neither the
# triangulator's reflex-vertex handling nor the broadphase nor a MAP_DATA
# transfer long enough to need more than one chunk.
node tools/mapgen/mapgen.js --cells 16 --seed 1 --out maps/big.map
"./build/$HOST_ARCH/mapc" maps/big.map    "build/$HOST_ARCH/big.dnpm"

# The Lua runtime, built separately from the core tests on purpose: those
# compile against core sources only, which is what lets the determinism gate run
# before any third-party toolchain is installed.
if [[ -f third_party/luajit/src/libluajit.a ]]; then
  ./scripts/build_lua_tests.sh >/dev/null
  "./build/lua/test_lua_vm"
  "./build/lua/test_lua_fx"
  "./build/lua/test_lua_effects"
else
  echo "  skip lua tests (run: scripts/setup_luajit.sh)"
fi

echo
echo "=== 3/5  sanitizers (ASan + UBSan) ==="
# Not redundant with the tests above. UBSan is what catches undefined behaviour
# that currently happens to compile into the intended instruction -- shifting a
# negative value left, signed overflow -- which is invisible until a compiler
# upgrade resolves it differently on one target and silently breaks prediction.
./scripts/check_sanitizers.sh

echo
echo "=== 4/5  cross-target determinism gate ==="
./scripts/check_determinism.sh

echo
echo "=== 5/5  transport + full stack ==="
if [[ ! -d server/edge/node_modules ]]; then
  echo "  skip (run: cd server/edge && npm install)"
elif [[ ! -f server/edge/certs/cert.pem ]]; then
  echo "  skip (run: cd server/edge && npm run certs)"
else
  # The dev certificate is valid for 13 days by design; an expired one fails the
  # handshake with no useful diagnostic, so say so plainly here instead.
  if ! openssl x509 -in server/edge/certs/cert.pem -checkend 0 -noout >/dev/null 2>&1; then
    echo "  dev certificate has EXPIRED -- regenerating"
    (cd server/edge && npm run --silent certs >/dev/null)
  fi
  ./scripts/build_server.sh >/dev/null

  # The wasm core, BEFORE the tests that load a map through it.
  #
  # It used to be built further down, next to the prediction tests, and the map
  # transfer test quietly ran against whatever binary happened to be on disk. A
  # map format change then failed here with "status 2" and no hint that the
  # client was simply older than the file it was parsing -- which is the exact
  # confusion the format version exists to prevent, reintroduced by the harness.
  if command -v em++ >/dev/null 2>&1; then
    ./scripts/build_wasm.sh >/dev/null
  fi

  node client/test/snapshots.test.js
  # Pure: no wasm, no GPU. The geometry rules of a Doom-style renderer are
  # arithmetic, and arithmetic is worth asserting rather than screenshotting.
  node client/test/mapgeom.test.js | tail -2
  node client/test/camera.test.js | tail -2
  node client/test/entities.test.js | tail -2
  node client/test/lighting.test.js | tail -2
  node client/test/shadows.test.js | tail -2
  node client/test/props.test.js | tail -2
  node client/test/swing.test.js | tail -2
  # And the same rules applied to the map the server actually ships, read
  # through the same wasm module the client predicts with.
  node client/test/mapmesh.test.js | tail -2
  (cd server/edge && node test/framing.test.js \
     && node test/relay.test.js | tail -2 \
     && node test/e2e.test.js 2>/dev/null | tail -2 \
     && node test/delta.test.js 2>/dev/null | tail -2 \
     && node test/mapdata.test.js 2>/dev/null | tail -2 \
     && DNP_MAP="$ROOT/build/$HOST_ARCH/big.dnpm" DNP_WT_PORT=34471 DNP_LINK_PORT=34541 \
        node test/mapdata.test.js 2>/dev/null | tail -2 \
     && node test/platform.test.js 2>/dev/null | tail -2 \
     && node test/crush.test.js 2>/dev/null | tail -2 \
     && node test/phase3.test.js 2>/dev/null | tail -2 \
     && node test/params.test.js 2>/dev/null | tail -2 \
     && node test/lua.test.js 2>/dev/null | tail -2 \
     && node test/doors.test.js 2>/dev/null | tail -2 \
     && node test/attack.test.js 2>/dev/null | tail -2 \
     && node test/levels.test.js 2>/dev/null | tail -2 \
     && node test/live.test.js 2>/dev/null | tail -2)

  # The control case: a client that ignores the replicated motion parameters
  # MUST mispredict. If it does not, the replication is doing nothing and the
  # test above proves nothing.
  echo
  echo "--- control: ignoring motion parameters must break prediction ---"
  (cd server/edge && DNP_IGNORE_MOTION=1 DNP_WT_PORT=34470 DNP_LINK_PORT=34540 \
     node test/platform.test.js 2>/dev/null | grep -E "CONTROL|platform:")
  (cd server/edge && DNP_IGNORE_MOTION=1 DNP_WT_PORT=34480 DNP_LINK_PORT=34550 \
     node test/crush.test.js 2>/dev/null | grep -E "CONTROL|crush:")
  # Sector motion is the map's business now: run without a script and nothing
  # moves, which is what proves the engine is not still driving it quietly.
  (cd server/edge && DNP_NO_SCRIPT=1 DNP_WT_PORT=34498 DNP_LINK_PORT=34582 \
     node test/phase3.test.js 2>/dev/null | grep -E "CONTROL|phase3:")
  # Doors: the same walk with the button never pressed must leave the door
  # shut, and with no script at all nothing may move. The first proves the door
  # is not on a timer; the second proves the engine reports rather than acts.
  (cd server/edge && DNP_NO_USE=1 DNP_WT_PORT=34493 DNP_LINK_PORT=34567 \
     node test/doors.test.js 2>/dev/null | grep -E "CONTROL|doors:")
  (cd server/edge && DNP_NO_SCRIPT=1 DNP_WT_PORT=34494 DNP_LINK_PORT=34568 \
     node test/doors.test.js 2>/dev/null | grep -E "CONTROL|doors:")
  # And the flag: a door is a change no client could predict, so a client that
  # ignores LUA_APPLIED must report it as a defect. If it did not, the flag
  # would be carrying no information.
  (cd server/edge && DNP_IGNORE_LUA=1 DNP_WT_PORT=34495 DNP_LINK_PORT=34569 \
     node test/doors.test.js 2>/dev/null | grep -E "CONTROL|doors:")
  # A solid prop that stops you, and a swing that removes it: without the swing
  # the same walk must end in the same place, or `solid` is doing nothing.
  (cd server/edge && DNP_NO_ATTACK=1 DNP_WT_PORT=34499 DNP_LINK_PORT=34573 \
     node test/attack.test.js 2>/dev/null | grep -E "CONTROL|attack:")
  # A map change must be a move to the NEXT level, not any reset at all: with
  # only one level loaded the exit has nowhere to go and must move nobody.
  (cd server/edge && DNP_ONE_LEVEL=1 DNP_WT_PORT=34497 DNP_LINK_PORT=34571 \
     node test/levels.test.js 2>/dev/null | grep -E "CONTROL|levels:")
  # And the other half of §3.1.2: a client that discards the STREAMED offsets
  # has nothing for anchor-relative prediction to be relative to.
  (cd server/edge && DNP_IGNORE_OFFSETS=1 DNP_WT_PORT=34486 DNP_LINK_PORT=34556 \
     node test/phase3.test.js 2>/dev/null | grep -E "CONTROL|phase3:")
  # And the rules themselves: a client that discards PARAM_UPDATE runs different
  # physics from the server and must be caught saying so.
  (cd server/edge && DNP_IGNORE_PARAMS=1 DNP_WT_PORT=34488 DNP_LINK_PORT=34558 \
     node test/params.test.js 2>/dev/null | grep -E "CONTROL|params:")
  # Scripted changes: one control proves the flag is not set unconditionally,
  # the other proves a client that ignores it reports a defect.
  (cd server/edge && DNP_NO_SCRIPT=1 DNP_WT_PORT=34492 DNP_LINK_PORT=34572 \
     node test/lua.test.js 2>/dev/null | grep -E "CONTROL|lua:")
  (cd server/edge && DNP_IGNORE_LUA=1 DNP_WT_PORT=34494 DNP_LINK_PORT=34574 \
     node test/lua.test.js 2>/dev/null | grep -E "CONTROL|lua:")
  # A scripted change to the RULES rather than the state: it travels on the
  # reliable channel with an epoch, not as a snapshot flag.
  (cd server/edge && DNP_EFFECTS=1 DNP_WT_PORT=34496 DNP_LINK_PORT=34578 \
     node test/lua.test.js 2>/dev/null | tail -2)

  echo
  echo "--- client prediction against the real server ---"
  if command -v em++ >/dev/null 2>&1; then
    (cd server/edge && node test/prediction.test.js 2>/dev/null | tail -4)

    echo
    echo "--- prediction under simulated network conditions ---"
    node tools/netsim/matrix.js --quick 2>/dev/null | tail -8
  else
    echo "  skip (emsdk not active -- source emsdk/emsdk_env.sh)"
    echo "  NOTE: this is the test that verifies client and server agree bit-for-bit."
  fi
fi

echo
echo "all checks passed"
