#!/usr/bin/env bash
#
# Bring the whole thing up: server, WebTransport edge, dev web server.
#
#   scripts/run_stack.sh [map] [--script <lua>]
#
# Then open http://localhost:8080/?server=https://127.0.0.1:34433/dnp
#
# Three processes rather than one because that is genuinely what the
# architecture is: the C++ server is transport-agnostic and speaks a local
# framed socket, the edge terminates WebTransport, and the page is static. The
# split was a deliberate Phase 1 decision (there is no batteries-included
# WebTransport server for C++) and hiding it behind a single binary would make
# the deployment story less honest, not simpler.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ARCH="$(uname -m)"
MAP="${1:-build/$ARCH/test.dnpm}"
shift || true

LINK_PORT="${DNP_LINK_PORT:-34500}"
WT_PORT="${DNP_WT_PORT:-34433}"
HTTP_PORT="${DNP_HTTP_PORT:-8080}"

# scripts/build_server.sh writes to build/server, not to the per-architecture
# directory the determinism gate uses -- the server is not part of that gate.
SERVER="build/server/dnp-server"
[[ -x "$SERVER" ]] || { echo "no server binary; run scripts/build_server.sh"; exit 1; }
[[ -f "$MAP" ]] || { echo "no map at $MAP; run mapc first"; exit 1; }
[[ -f server/edge/certs/cert-hash.json ]] || {
  echo "no dev certificate; run: (cd server/edge && npm run certs)"; exit 1; }
[[ -f client/wasm/dist/dnp_core.mjs ]] || { echo "no wasm build; run scripts/build_wasm.sh"; exit 1; }

pids=()
cleanup () { for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done; }
trap cleanup EXIT INT TERM

echo "[stack] server  : $MAP  (link 127.0.0.1:$LINK_PORT)"
"$SERVER" --port "$LINK_PORT" --map "$MAP" "$@" &
pids+=($!)

sleep 0.6

echo "[stack] edge    : https://127.0.0.1:$WT_PORT/dnp"
( cd server/edge && node src/index.js --port "$WT_PORT" --upstream-port "$LINK_PORT" ) &
pids+=($!)

echo "[stack] web     : http://localhost:$HTTP_PORT/"
DNP_HTTP_PORT="$HTTP_PORT" node tools/devserver.js &
pids+=($!)

echo
echo "  open  http://localhost:$HTTP_PORT/?server=https://127.0.0.1:$WT_PORT/dnp"
echo "  (no ?server= gives the solo renderer, which needs none of the above)"
echo
wait
