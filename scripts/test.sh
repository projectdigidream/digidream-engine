#!/usr/bin/env bash
#
# Runs everything CI runs, in the order that fails cheapest-first.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "=== 1/3  no-floats guard ==="
./scripts/check_no_floats.sh

echo
echo "=== 2/3  core/math unit + property tests ==="
HOST_ARCH="$(uname -m)"
./scripts/build_native.sh "$HOST_ARCH" >/dev/null
"./build/$HOST_ARCH/test_math"

echo
echo "=== 3/3  cross-target determinism gate ==="
./scripts/check_determinism.sh

echo
echo "all checks passed"
