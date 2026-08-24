#!/usr/bin/env bash
#
# Installs a PINNED Emscripten SDK into ./emsdk (gitignored).
#
# The version is pinned deliberately. The wasm build is one half of the
# determinism comparison, so an unpinned toolchain would mean CI could start
# failing -- or worse, start passing differently -- because of an upstream
# release nobody in this repository chose. When bumping it, re-run
# scripts/check_determinism.sh and record the new final hash in
# docs/physics-contract.md.
#
set -euo pipefail

EMSDK_VERSION="${EMSDK_VERSION:-6.0.8}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ ! -d emsdk ]]; then
  git clone https://github.com/emscripten-core/emsdk.git emsdk
fi

cd emsdk
git fetch --tags
./emsdk install "$EMSDK_VERSION"
./emsdk activate "$EMSDK_VERSION"

cat <<EOF

emsdk $EMSDK_VERSION installed.

Activate it in your shell with:

    source "$ROOT/emsdk/emsdk_env.sh"

then re-run scripts/check_determinism.sh -- it will pick up em++ automatically
and bring wasm32 under the gate.
EOF
