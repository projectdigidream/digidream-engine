#!/usr/bin/env bash
#
# Runs everything CI runs, in the order that fails cheapest-first.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "=== 1/5  no-floats guard ==="
./scripts/check_no_floats.sh

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

# The map compiler must round-trip the authored map: mapc bakes it, and the
# loader re-triangulates and compares, so this also proves the compiler and the
# runtime have not drifted apart.
"./build/$HOST_ARCH/mapc" maps/test.map  "build/$HOST_ARCH/test.dnpm"
"./build/$HOST_ARCH/mapc" maps/crush.map "build/$HOST_ARCH/crush.dnpm"

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
  node client/test/snapshots.test.js
  (cd server/edge && node test/framing.test.js \
     && node test/relay.test.js | tail -2 \
     && node test/e2e.test.js 2>/dev/null | tail -2 \
     && node test/delta.test.js 2>/dev/null | tail -2 \
     && node test/mapdata.test.js 2>/dev/null | tail -2 \
     && node test/platform.test.js 2>/dev/null | tail -2 \
     && node test/crush.test.js 2>/dev/null | tail -2)

  # The control case: a client that ignores the replicated motion parameters
  # MUST mispredict. If it does not, the replication is doing nothing and the
  # test above proves nothing.
  echo
  echo "--- control: ignoring motion parameters must break prediction ---"
  (cd server/edge && DNP_IGNORE_MOTION=1 DNP_WT_PORT=34470 DNP_LINK_PORT=34540 \
     node test/platform.test.js 2>/dev/null | grep -E "CONTROL|platform:")
  (cd server/edge && DNP_IGNORE_MOTION=1 DNP_WT_PORT=34480 DNP_LINK_PORT=34550 \
     node test/crush.test.js 2>/dev/null | grep -E "CONTROL|crush:")

  echo
  echo "--- client prediction against the real server ---"
  if command -v em++ >/dev/null 2>&1; then
    ./scripts/build_wasm.sh >/dev/null
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
