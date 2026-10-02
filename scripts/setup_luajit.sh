#!/usr/bin/env bash
#
# Installs a PINNED LuaJIT into ./third_party/luajit (gitignored).
#
# Pinned to a COMMIT, not a branch or a tag. A tag is a moving reference in
# principle and this is the interpreter that will run untrusted map scripts:
# "whatever upstream tagged most recently" is not an acceptable answer to which
# sandbox is in force. The same reasoning as scripts/setup_emsdk.sh, one notch
# stricter, because this one is a security boundary and not only a toolchain.
#
# The fork is OpenResty's LuaJIT2 rather than the original: it is actively
# maintained, and its arm64 macOS support is the reason this builds on the
# machine it was written on.
#
# LuaJIT is linked as ONE STATIC ARCHIVE, deliberately. Adding ~40 .c files to
# the six hand-maintained source lists in this repo is how those lists rot, and
# scripts/check_source_lists.sh exists to prove they have not. An archive means
# exactly one build script changes: scripts/build_server.sh.
#
set -euo pipefail

# openresty/luajit2 v2.1-20260824
LUAJIT_COMMIT="${LUAJIT_COMMIT:-fbfc558aacd57a54623df0ced4c31a28f81f8ff2}"
LUAJIT_REPO="${LUAJIT_REPO:-https://github.com/openresty/luajit2.git}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The repository. The engine is $ROOT; modules, the studio, the synth and
# the rest are beside it, here.
REPO="$(cd "$ROOT/.." && pwd)"
DEST="$ROOT/third_party/luajit"

mkdir -p "$ROOT/third_party"

if [[ ! -d "$DEST/.git" ]]; then
  echo "[luajit] cloning $LUAJIT_REPO"
  git clone --filter=blob:none "$LUAJIT_REPO" "$DEST"
fi

cd "$DEST"
git fetch --depth 1 origin "$LUAJIT_COMMIT" 2>/dev/null || git fetch origin
git checkout --detach "$LUAJIT_COMMIT"

# Refuse to build anything other than the pin. A silently-different interpreter
# is exactly the failure this script exists to prevent.
HAVE="$(git rev-parse HEAD)"
if [[ "$HAVE" != "$LUAJIT_COMMIT" ]]; then
  echo "[luajit] FAILED to check out the pin: wanted $LUAJIT_COMMIT, got $HAVE" >&2
  exit 1
fi

# Without this LuaJIT's makefile refuses to build on macOS.
#
# A conservative floor, NOT the host's version. Using `sw_vers` looks tidier and
# is wrong: clang targets the installed SDK (15.0 here), so a LuaJIT built for
# the host's 26.0 makes the linker warn "built for newer macOS than being
# linked" on every single server build. A floor below both is compatible with
# both and silent.
if [[ "$(uname -s)" == "Darwin" ]]; then
  export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
  echo "[luajit] MACOSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET"
fi

echo "[luajit] building (amalgamated)"
make clean >/dev/null 2>&1 || true
make amalg -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" >/dev/null

if [[ ! -f "$DEST/src/libluajit.a" ]]; then
  echo "[luajit] build produced no libluajit.a" >&2
  exit 1
fi

cat <<EOF

LuaJIT installed.

  commit   $LUAJIT_COMMIT
  archive  $DEST/src/libluajit.a
  headers  $DEST/src

Nothing else in the build changes: only scripts/build_server.sh links it, which
scripts/check_source_lists.sh enforces. If you bump the pin, re-run
./scripts/test.sh -- the determinism gate must be unaffected, and if it is not,
Lua has leaked into a target it must never reach.
EOF
