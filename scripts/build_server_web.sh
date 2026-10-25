#!/usr/bin/env bash
#
# The server as WebAssembly, for the single-player web build (docs/WEB.md):
# client/wasm/dist/dnp-server.mjs and .wasm, beside the client's own wasm core:
# an ES module the page's Worker runs (client/src/server_worker.js).
#
# The same sources as the native server -- this is scripts/build_server.sh with
# em++, PUC Lua 5.1 built for wasm (LuaJIT has no wasm port), and the link's
# socket swapped for two byte queues (server/src/net/link.cpp, __EMSCRIPTEN__).
# main() sets the server up and returns with it running; the Worker turns it
# with dnp_web_pump and moves link bytes with dnp_web_feed / dnp_web_take.
#
#   source emsdk/emsdk_env.sh && engine/scripts/build_server_web.sh
#
# WASM EXCEPTIONS (-fwasm-exceptions), because Lua is built as C++ and raises a
# Lua error as a C++ exception (setup_lua51.sh says why). Every current browser
# and Node 22 run them. No threads, so no SharedArrayBuffer and no special
# headers: a static host serves it as it is.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
command -v em++ >/dev/null || { echo "[web] no em++ -- source emsdk/emsdk_env.sh first" >&2; exit 2; }

if [[ ! -f "$ROOT/third_party/lua51/build/wasm/liblua.a" ]]; then
  CXX=em++ AR=emar CXXFLAGS="-fwasm-exceptions" "$ROOT/scripts/setup_lua51.sh" wasm
fi

# -g0 after build_server.sh's -g: DWARF would make the wasm tens of megabytes.
DNP_LUA=lua51 DNP_LUA51_BUILD=wasm \
DNP_SERVER_CXX=em++ DNP_SERVER_SYSLIBS="" \
DNP_SERVER_CXXFLAGS="-fwasm-exceptions -g0" \
DNP_SERVER_EXE=dnp-server.mjs DNP_SERVER_OUT="$ROOT/client/wasm/dist" \
DNP_SERVER_LDFLAGS="-fwasm-exceptions -sMODULARIZE -sEXPORT_ES6 -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 \
  -sALLOW_MEMORY_GROWTH -sSTACK_SIZE=8MB -sENVIRONMENT=worker,node \
  -sEXPORTED_RUNTIME_METHODS=FS,callMain,HEAPU8 \
  -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_dnp_web_pump,_dnp_web_feed,_dnp_web_take" \
  "$ROOT/scripts/build_server.sh"
