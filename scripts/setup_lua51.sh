#!/usr/bin/env bash
#
# Installs PUC Lua 5.1.5 into ./third_party/lua51 (gitignored) and builds it as
# one static archive, third_party/lua51/build/<name>/liblua.a.
#
# WHY A SECOND LUA. LuaJIT cannot run in WebAssembly -- its interpreter is
# hand-written assembly per CPU -- and the single-player web build runs the
# server in wasm. PUC Lua 5.1 has the C API LuaJIT implements (setfenv
# included, which the script sandbox is built on), and the server already runs
# LuaJIT with its JIT off, so this is interpreter against interpreter.
# The native build against it (scripts/build_server.sh and build_lua_tests.sh
# with DNP_LUA=lua51) is how the scripts are kept working on it.
#
#   scripts/setup_lua51.sh                 the host's archive (build/host)
#   CXX=em++ AR=emar scripts/setup_lua51.sh wasm   the web build's (build/wasm)
#
# Pinned to the release tarball's SHA-256, as lua.org lists it. With it, Lua
# BitOp (bitop.luajit.org, MIT): the `bit` library LuaJIT has built in and the
# scripts use; its site lists no checksum, so this is the one of the release
# that was fetched and read (bit.c, 191 lines) on 2026-10-25.
set -euo pipefail

VERSION=5.1.5
SHA256=2640fc56a795f29d28ef15e13c34a47e223960b0240e8cb0a82d9b0738695333
BITOP=1.0.4
BITOP_SHA256=adc32785579fc62f7736881332f70f50fe168b2ae8d0ddb741ff018a76f48c80
NAME="${1:-host}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/third_party/lua51"
SRC="$DEST/lua-$VERSION/src"
OUT="$DEST/build/$NAME"
CXX="${CXX:-c++}"
AR="${AR:-ar}"

if [[ ! -f "$SRC/lua.h" ]]; then
  mkdir -p "$DEST"
  tgz="$DEST/lua-$VERSION.tar.gz"
  echo "[lua51] fetching lua-$VERSION"
  curl -sSfL -o "$tgz" "https://www.lua.org/ftp/lua-$VERSION.tar.gz"
  have="$(shasum -a 256 "$tgz" | cut -d' ' -f1)"
  if [[ "$have" != "$SHA256" ]]; then
    echo "[lua51] FAILED: lua-$VERSION.tar.gz is $have, wanted $SHA256" >&2
    rm -f "$tgz"
    exit 1
  fi
  tar -xzf "$tgz" -C "$DEST"
fi
if [[ ! -f "$DEST/LuaBitOp-$BITOP/bit.c" ]]; then
  tgz="$DEST/LuaBitOp-$BITOP.tar.gz"
  echo "[lua51] fetching LuaBitOp-$BITOP"
  curl -sSfL -o "$tgz" "https://bitop.luajit.org/download/LuaBitOp-$BITOP.tar.gz"
  have="$(shasum -a 256 "$tgz" | cut -d' ' -f1)"
  if [[ "$have" != "$BITOP_SHA256" ]]; then
    echo "[lua51] FAILED: LuaBitOp-$BITOP.tar.gz is $have, wanted $BITOP_SHA256" >&2
    rm -f "$tgz"
    exit 1
  fi
  tar -xzf "$tgz" -C "$DEST"
fi

# AS C++. Compiled as C, Lua raises an error by longjmp, which skips the
# destructors of every C++ frame between the error and the pcall -- and the
# bindings raise from C++ that holds strings and vectors, which LuaJIT unwinds
# properly. Corrupted heaps followed (a test's chunks refused as bad syntax,
# then lua_close hanging). Compiled as C++, Lua 5.1 throws instead (luaconf.h,
# LUAI_THROW), and inside `extern "C"` its API keeps C linkage, so the server
# links it exactly as it links LuaJIT. One small wrapper per file (Lua's own
# one-file build, etc/all.c, leans on C's tentative definitions); the C headers
# go first, outside `extern "C"`, because libc++'s <math.h> holds templates.
mkdir -p "$OUT"
rm -f "$OUT"/*.o "$OUT"/*.cpp "$OUT/liblua.a"
for c in lapi lcode ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring ltable \
         ltm lundump lvm lzio lauxlib lbaselib ldblib liolib linit lmathlib loadlib loslib lstrlib ltablib bit; do
  file="$SRC/$c.c"
  [[ $c == bit ]] && file="$DEST/LuaBitOp-$BITOP/bit.c"
  {
    for h in assert ctype errno float limits locale math setjmp signal stdarg stddef stdint stdio stdlib string time; do
      echo "#include <$h.h>"
    done
    echo 'extern "C" {'
    echo "#include \"$file\""
    echo '}'
  } > "$OUT/$c.cpp"
  "$CXX" -x c++ -std=c++17 -O2 -DLUA_USE_POSIX -w ${CXXFLAGS:-} -I"$SRC" -c "$OUT/$c.cpp" -o "$OUT/$c.o"
done
"$AR" rcs "$OUT/liblua.a" "$OUT"/*.o
rm -f "$OUT"/*.o "$OUT"/*.cpp
echo "[lua51] $OUT/liblua.a"
