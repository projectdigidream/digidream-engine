#!/usr/bin/env bash
#
# Package the VS Code extension as a .vsix you can install normally.
#
#   scripts/build_vsix.sh                  -> build/doomnetplatformer-<version>.vsix
#   DNP_VSIX_OUT=/somewhere/x.vsix scripts/build_vsix.sh
#
# Then in VS Code: Extensions -> "..." -> Install from VSIX...
#
# WHAT THIS ADDS TO THE FOLDER, and why there is a step at all. The extension
# imports the engine-root rule, tools/engineroot.js, and inside the repository
# it reaches that file as ../tools/engineroot.js. An installed extension is a
# COPY under ~/.vscode/extensions/ with nothing beside it, so the file is
# bundled in. It is copied fresh from tools/ on every build rather than kept
# as a second checked-in copy, and the extension compares the two at runtime
# and says so if they have drifted.
#
# vsce is the packager rather than a hand-written manifest: the .vsix format
# has a manifest VS Code is particular about, and a file this script cannot
# check against a real VS Code should be produced by the tool that can. It is
# fetched on demand through npx, pinned to a major version, the way
# app/ensure-electron.cjs fetches Electron -- nothing is added to the
# repository's dependencies for a step most people never run.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The repository. The engine is $ROOT; modules, the studio, the synth and
# the rest are beside it, here.
REPO="$(cd "$ROOT/.." && pwd)"
SRC="$REPO/vscode-extension"
STAGE="$ROOT/build/vsix/src"

VERSION="$(node -p "require('$SRC/package.json').version")"
OUT="${DNP_VSIX_OUT:-$ROOT/build/doomnetplatformer-$VERSION.vsix}"

rm -rf "$STAGE"
mkdir -p "$STAGE" "$(dirname "$OUT")"

# Exactly the files the extension is made of, plus the one it borrows.
cp "$SRC/package.json" "$SRC/extension.cjs" "$SRC/host.js" "$SRC/README.md" "$STAGE/"
cp "$REPO/editor/engineroot.js" "$STAGE/engineroot.js"
# No LICENSE file: the repository has none and the manifest says UNLICENSED,
# which is what a private package is. --skip-license below stops vsce asking.

cd "$STAGE"
# --no-dependencies: there is no node_modules and nothing to bundle.
# --allow-missing-repository: the manifest names no repository URL, on purpose.
npx --yes @vscode/vsce@4 package \
  --no-dependencies \
  --allow-missing-repository \
  --skip-license \
  --out "$OUT"

echo
echo "built $OUT"
echo "install: VS Code -> Extensions -> ... -> Install from VSIX..."
