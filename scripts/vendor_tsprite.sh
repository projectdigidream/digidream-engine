#!/usr/bin/env bash
#
# Vendors ThreeSprite's runtime into the studio, as one file.
#
# The studio imports a .tsprite project by COMPOSING it -- posing the rig,
# rendering every view of every frame, packing the sheet -- and that is
# ThreeSprite's own rasteriser, not a second one: @threesprite/runtime's
# `tsprite` entry point bundles the editor's code for exactly this. Its own
# build leaves the zip and PNG libraries as dependencies; this bundles them in,
# because the studio is served as plain files and has no package install.
#
#   scripts/vendor_tsprite.sh                 # from ../ThreeSprite
#   THREESPRITE=/path/to/ThreeSprite scripts/vendor_tsprite.sh
#
# The result, studio/vendor/tsprite.js, IS committed: it is the runtime the
# studio depends on, MIT-licensed, and rebuilding it needs that checkout.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The repository. The engine is $ROOT; modules, the studio, the synth and
# the rest are beside it, here.
REPO="$(cd "$ROOT/.." && pwd)"
TS="${THREESPRITE:-$ROOT/../ThreeSprite}"
[[ -f "$TS/runtime/src/tsprite.ts" ]] || { echo "[vendor] no ThreeSprite checkout at $TS (set THREESPRITE)" >&2; exit 2; }
ESB="$(ls "$TS/runtime/node_modules/.bin/esbuild" 2>/dev/null || ls -d "$TS"/node_modules/.pnpm/esbuild@*/node_modules/esbuild/bin/esbuild 2>/dev/null | head -1 || true)"
[[ -n "$ESB" && -x "$ESB" ]] || { echo "[vendor] esbuild not found under $TS (run pnpm install there)" >&2; exit 2; }
mkdir -p "$REPO/studio/vendor"
OUT="$REPO/studio/vendor/tsprite.js"
COMMIT="$(git -C "$TS" rev-parse --short HEAD 2>/dev/null || echo unknown)"
{
  echo "// @threesprite/runtime -- the .tsprite entry point, bundled for the browser."
  echo "// MIT. Built by scripts/vendor_tsprite.sh from ThreeSprite $COMMIT; do not edit here."
  "$ESB" "$TS/runtime/src/tsprite.ts" --bundle --format=esm --platform=browser --target=es2020 --log-level=warning
} > "$OUT"
echo "[vendor] $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes, ThreeSprite $COMMIT)"
