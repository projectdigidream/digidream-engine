#!/usr/bin/env bash
#
# Runs everything CI runs, in the order that fails cheapest-first.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "=== 1/4  no-floats guard ==="
./scripts/check_no_floats.sh

echo
echo "=== 2/4  core/math unit + property tests ==="
HOST_ARCH="$(uname -m)"
./scripts/build_native.sh "$HOST_ARCH" >/dev/null
"./build/$HOST_ARCH/test_math"

echo
echo "=== 3/4  cross-target determinism gate ==="
./scripts/check_determinism.sh

echo
echo "=== 4/4  edge transport ==="
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
  (cd server/edge && node test/framing.test.js && node test/relay.test.js | tail -2)
fi

echo
echo "all checks passed"
