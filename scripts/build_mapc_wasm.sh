#!/usr/bin/env bash
#
# Builds the map compiler to WebAssembly, for the studio.
#
# THE SAME mapc, not a port. tools/mapc/main.cpp is the only definition of the
# .map grammar and the only place its validation lives; the studio bakes on a
# phone by running that file, so a level baked on the device is byte-identical
# to one baked on a workstation and fails for the same reasons, on the same
# line numbers. A JavaScript parser that agreed with mapc would agree until it
# did not.
#
# THE CORE SOURCE LIST IS scripts/build_wasm.sh's, read out of it rather than
# copied -- the same arrangement scripts/build_apk.sh has with build_server.sh.
# scripts/check_source_lists.sh keeps six hand-written lists in agreement, and
# a seventh that cannot drift is better than a seventh it has to check.
#
# The flags mirror build_wasm.sh: the triangulation this bakes has to be the
# one the runtime recomputes on load, or every map fails TriangleMismatch.
#
set -euo pipefail

if ! command -v em++ >/dev/null 2>&1; then
  echo "em++ not found -- run scripts/setup_emsdk.sh then: source emsdk/emsdk_env.sh" >&2
  exit 1
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The repository. The engine is $ROOT; modules, the studio, the synth and
# the rest are beside it, here.
REPO="$(cd "$ROOT/.." && pwd)"
OUT="$REPO/studio/wasm/dist"
mkdir -p "$OUT"

# `read` in a loop rather than `mapfile`: macOS ships bash 3.2.
SRCS=()
while IFS= read -r f; do SRCS+=("$ROOT/$f"); done < <(
  sed -n 's|^  "\$ROOT/\(core/src/.*\.cpp\)" \\$|\1|p' "$ROOT/scripts/build_wasm.sh")
(( ${#SRCS[@]} > 5 )) || {
  echo "[mapc-wasm] could not read the core list out of scripts/build_wasm.sh" >&2
  echo "            (found ${#SRCS[@]} files; its shape must have changed)" >&2
  exit 2; }

# STACK_SIZE: mapc keeps its MapStorage as a stack local, about half a
# megabyte, and Emscripten's default stack is 64 KB. Without this the bake
# overruns the stack into the heap and fails somewhere else entirely.
#
# INVOKE_RUN=0 + EXIT_RUNTIME=0: main() is called by the studio, per bake, on a
# fresh instance -- mapc counts its errors in a global it never resets, and a
# new instance is cheaper than teaching it to.
em++ -std=c++17 -O2 \
  -Wall -Wextra -Wpedantic -Werror \
  -fno-fast-math -ffp-contract=off \
  -I"$ROOT/core/include" \
  "${SRCS[@]}" \
  "$ROOT/tools/mapc/main.cpp" \
  -sMODULARIZE=1 \
  -sEXPORT_ES6=1 \
  -sENVIRONMENT=node,web \
  -sALLOW_MEMORY_GROWTH=1 \
  -sSTACK_SIZE=2097152 \
  -sFORCE_FILESYSTEM=1 \
  -sINVOKE_RUN=0 \
  -sEXIT_RUNTIME=0 \
  -sEXPORTED_RUNTIME_METHODS=FS,callMain \
  -sEXPORTED_FUNCTIONS=_main \
  -o "$OUT/mapc.mjs"

echo "[build] $OUT/mapc.mjs"
