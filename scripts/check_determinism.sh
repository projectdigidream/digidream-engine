#!/usr/bin/env bash
#
# THE determinism gate. Builds the golden trace for every target available on
# this machine and asserts all of them emit a byte-identical hash stream.
#
# Client-side prediction (design 9.3) is only correct because the browser's wasm
# build and the server's native build run the same physics and reach the same
# answer. If that stops holding, prediction degrades into permanent mispredicts
# with no gameplay cause -- and the symptom (rubber-banding under load) looks
# nothing like the cause. This script is how that is caught on the commit that
# introduces it rather than months later.
#
# Targets are skipped, not failed, when their toolchain is absent, so this runs
# usefully on a laptop and exhaustively in CI.
#
set -euo pipefail

TICKS="${TICKS:-10000}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The repository. The engine is $ROOT; modules, the studio, the synth and
# the rest are beside it, here.
REPO="$(cd "$ROOT/.." && pwd)"
WORK="$ROOT/build/determinism"
mkdir -p "$WORK"

FLAGS=(-std=c++17 -O2 -Wall -Wextra -Werror -fno-fast-math -ffp-contract=off -I"$ROOT/core/include")
SRCS=("$ROOT/core/src/math/trig.cpp" "$ROOT/core/src/proto/messages.cpp" "$ROOT/core/src/sim/world.cpp" "$ROOT/core/src/sim/testmap.cpp" "$ROOT/core/src/sim/sectormotion.cpp" "$ROOT/core/src/sim/weapon.cpp" "$ROOT/core/src/sim/crush.cpp" "$ROOT/core/src/geom/map.cpp" "$ROOT/core/src/geom/triangulate.cpp" "$ROOT/core/src/geom/mapstore.cpp" "$ROOT/core/src/geom/mapfile.cpp" "$ROOT/core/src/geom/rail.cpp" "$ROOT/core/src/physics/collide.cpp" "$ROOT/core/src/physics/controller.cpp" "$ROOT/core/src/nav/navgraph.cpp" "$ROOT/core/tests/golden_trace.cpp")

declare -a NAMES=()
declare -a TRACES=()

emit() { NAMES+=("$1"); TRACES+=("$2"); }

# Assert a gate binary contains no Lua.
#
# scripts/check_source_lists.sh already asserts no build script under the gate
# LINKS Lua, and that core/ never mentions it. This is the same claim checked
# from the other end, against the artifact rather than against its recipe -- a
# source grep can be walked around with an include path or a symlink, and a
# build-script grep only sees the flags someone wrote down. The symbol table is
# what actually got compiled.
#
# It matters because the failure is quiet: a golden_trace with Lua in it still
# builds, still runs, and still agrees with itself across targets. What it stops
# being is the thing the client runs.
assert_lua_free() {
  local label="$1" bin="$2"
  [[ -f "$bin" ]] || return 0
  # The character class is `[[:space:]_]`, not `\b`, and that is the whole
  # trick: Mach-O prefixes C symbols with an underscore, so the real name is
  # `_luaL_newstate` -- and `\b` never matches there, because `_` is itself a
  # word character. Written with \b this check silently passed a binary that
  # exported luaL_newstate and lua_pcall. The control in scripts/ is what found
  # that; a guard nobody has watched fail is not a guard.
  local pat='[[:space:]_](lua[LJ]?_|luaopen_)'
  if nm -a "$bin" 2>/dev/null | grep -qE "$pat"; then
    echo "  FAIL $label contains Lua symbols:"
    nm -a "$bin" 2>/dev/null | grep -oE '(lua[LJ]?_|luaopen_)[A-Za-z0-9_]*' | sort -u | head -8 | sed 's/^/        /'
    echo "       The determinism gate must compare the binary the CLIENT runs."
    echo "       The client never runs server Lua."
    LUA_LEAKED=1
  fi
}
LUA_LEAKED=0

# Self-test: prove the detector can detect, before trusting it to say "clean".
#
# Written after the first version of assert_lua_free() passed a binary that
# exported luaL_newstate and lua_pcall -- it used \b, which cannot match the
# leading underscore Mach-O puts on every C symbol. It reported every target
# clean, which was true and meaningless.
#
# A guard nobody has watched fail is indistinguishable from no guard, so this
# builds a deliberate leak and requires it to be caught.
selftest_lua_detector() {
  local probe="$WORK/lua_probe"
  cat > "$WORK/lua_probe.cpp" <<'PROBE'
extern "C" void* luaL_newstate() { return nullptr; }
extern "C" int lua_pcall(void*, int, int, int) { return 0; }
int main() { luaL_newstate(); return lua_pcall(nullptr, 0, 0, 0); }
PROBE
  if ! clang++ -std=c++17 -O0 "$WORK/lua_probe.cpp" -o "$probe" 2>/dev/null; then
    echo "  skip lua-detector self-test (probe did not build)"
    return 0
  fi
  if ! nm -a "$probe" 2>/dev/null | grep -qE '[[:space:]_](lua[LJ]?_|luaopen_)'; then
    echo "FAIL: the Lua symbol detector does not detect Lua symbols."
    echo "      It would report every target clean. Fix assert_lua_free()."
    exit 1
  fi
}
selftest_lua_detector

# --- native targets -------------------------------------------------------
for arch in arm64 x86_64; do
  if clang++ "${FLAGS[@]}" -arch "$arch" "${SRCS[@]}" -o "$WORK/gt_$arch" 2>/dev/null; then
    assert_lua_free "native-$arch" "$WORK/gt_$arch"
    if "$WORK/gt_$arch" 1 >/dev/null 2>&1; then
      # The chunk trace first (maps added and removed from a running world),
      # then the main one, so the last line is still the main trace's final.
      { "$WORK/gt_$arch" --chunks; "$WORK/gt_$arch" "$TICKS"; } > "$WORK/trace_$arch.txt"
      emit "native-$arch" "$WORK/trace_$arch.txt"
    else
      echo "  skip native-$arch (builds, but cannot execute here)"
    fi
  else
    echo "  skip native-$arch (no cross-compile support)"
  fi
done

# --- wasm -----------------------------------------------------------------
# The client target. Until emsdk is installed this is skipped, which means the
# gate is only partially enforced -- installing emsdk is a Phase 1 prerequisite,
# not an optional nicety.
if command -v em++ >/dev/null 2>&1 && command -v node >/dev/null 2>&1; then
  em++ "${FLAGS[@]}" -sENVIRONMENT=node -sALLOW_MEMORY_GROWTH=1 \
       "${SRCS[@]}" -o "$WORK/gt_wasm.js" 2>/dev/null
  # The wasm module has no Mach-O/ELF symbol table for nm to read, so this
  # checks the emitted binary and its JS glue for the names directly. Cruder
  # than nm and honestly so -- a stripped build could hide them -- which is why
  # it is the third guard on this property and not the only one.
  if grep -qaE 'lua[LJ]?_[a-z]|luaopen_' "$WORK/gt_wasm.wasm" "$WORK/gt_wasm.js" 2>/dev/null; then
    echo "  FAIL wasm32 contains Lua. The browser client must never link it."
    LUA_LEAKED=1
  fi
  { node "$WORK/gt_wasm.js" --chunks; node "$WORK/gt_wasm.js" "$TICKS"; } > "$WORK/trace_wasm.txt"
  emit "wasm32" "$WORK/trace_wasm.txt"
else
  echo "  skip wasm32 (emsdk not installed -- run scripts/setup_emsdk.sh)"
fi

# --- compare --------------------------------------------------------------
if [[ "$LUA_LEAKED" -ne 0 ]]; then
  echo "DETERMINISM GATE FAILED: Lua reached a gate target"
  exit 1
fi

COUNT=${#NAMES[@]}
if [[ "$COUNT" -lt 2 ]]; then
  echo "FAIL: only $COUNT target(s) available; determinism cannot be verified."
  exit 1
fi

REF_NAME="${NAMES[0]}"
REF_TRACE="${TRACES[0]}"
FAILED=0

for ((i = 1; i < COUNT; ++i)); do
  if diff -q "$REF_TRACE" "${TRACES[$i]}" >/dev/null; then
    echo "  ok   ${NAMES[$i]} == $REF_NAME"
  else
    FAILED=1
    # awk, not `diff --unchanged-line-format`: that flag is GNU-only and BSD
    # diff (macOS) rejects it outright.
    FIRST=$(awk 'NR==FNR { a[FNR] = $0; n = FNR; next }
                 { if (FNR > n || $0 != a[FNR]) { print $1; exit } }' \
              "$REF_TRACE" "${TRACES[$i]}")
    echo "  FAIL ${NAMES[$i]} diverges from $REF_NAME at tick ${FIRST:-?}"
    echo "       bisect with: scripts/bisect_divergence.sh $REF_TRACE ${TRACES[$i]}"
  fi
done

if [[ "$FAILED" -ne 0 ]]; then
  echo "DETERMINISM GATE FAILED"
  exit 1
fi

echo "determinism ok: $COUNT targets, $TICKS ticks, identical hash streams"
echo "  final $(tail -1 "$REF_TRACE")"
