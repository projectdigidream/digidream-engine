#!/usr/bin/env bash
#
# The core source list is written out by hand in six places. This proves they
# still agree, and that Lua is linked into exactly one of them.
#
# Why this exists now: until Phase 4 the six lists were identical by
# construction -- every one of them built the whole core and nothing else, so a
# mistake showed up as a link error on the next build. Lua breaks that. It
# belongs in the server build and MUST NOT appear in the three that feed the
# determinism gate, so for the first time the lists are required to differ in a
# specific way. "Differ in a specific way" is not something a link error can
# check for you.
#
# The failure this prevents is not a broken build. It is a QUIET one: Lua
# linked into golden_trace or the wasm client would still compile, still run,
# and still pass -- while the thing the gate compares is no longer the thing
# the client runs.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# The six. Named individually rather than globbed: a new build script must be
# added here deliberately, because the question "should this one link Lua?" has
# no safe default.
CORE_LISTS=(
  CMakeLists.txt
  scripts/build_server.sh
  scripts/build_native.sh
  scripts/build_wasm.sh
  scripts/check_sanitizers.sh
  scripts/check_determinism.sh
)

# Builds that feed the determinism gate or the browser client. Lua must be
# absent from every one of these.
GATE_LISTS=(
  scripts/build_native.sh
  scripts/build_wasm.sh
  scripts/check_sanitizers.sh
  scripts/check_determinism.sh
  CMakeLists.txt
)

FAILED=0

# --- 1. every list names the same core sources -----------------------------
#
# Extracted by pattern rather than by parsing each file's own syntax: the six
# use four different notations (CMake list, bash array, backslash continuation,
# one-line array) and a parser per notation would be more code than the thing
# it guards.
extract() { grep -oE 'core/src/[A-Za-z0-9_/]+\.cpp' "$1" | sort -u; }

REFERENCE="${CORE_LISTS[0]}"
REF_LIST="$(extract "$REFERENCE")"
REF_COUNT="$(printf '%s\n' "$REF_LIST" | grep -c . || true)"

if [[ "$REF_COUNT" -lt 5 ]]; then
  echo "FAIL: only $REF_COUNT core sources found in $REFERENCE -- the extractor is broken," >&2
  echo "      not the lists. A guard that matches nothing passes everything." >&2
  exit 1
fi

for f in "${CORE_LISTS[@]}"; do
  GOT="$(extract "$f")"
  if [[ "$GOT" != "$REF_LIST" ]]; then
    FAILED=1
    echo "FAIL: $f lists different core sources than $REFERENCE"
    # awk, not sed: these are file PATHS, and a path in the replacement text
    # ends the s/// expression at its first slash.
    diff <(printf '%s\n' "$REF_LIST") <(printf '%s\n' "$GOT") \
      | awk -v a="$REFERENCE" -v b="$f" '
          /^< / { print "  only in " a ": " substr($0, 3) }
          /^> / { print "  only in " b ": " substr($0, 3) }' || true
  fi
done

# --- 2. Lua is linked in exactly one place ---------------------------------
LUA_IN=()
for f in "${CORE_LISTS[@]}"; do
  if grep -qE 'luajit|lua\.h|third_party/luajit' "$f"; then
    LUA_IN+=("$f")
  fi
done

for f in "${GATE_LISTS[@]}"; do
  if grep -qE 'luajit|lua\.h|third_party/luajit' "$f"; then
    FAILED=1
    echo "FAIL: $f links Lua."
    echo "      This build feeds the determinism gate or the browser client, and the"
    echo "      client never runs server Lua. Linking it here means the gate is no"
    echo "      longer comparing the binary the client actually runs."
  fi
done

if [[ "${#LUA_IN[@]}" -gt 1 ]]; then
  FAILED=1
  echo "FAIL: Lua is linked in ${#LUA_IN[@]} build scripts: ${LUA_IN[*]}"
fi

# --- 3. core/ never mentions Lua at all ------------------------------------
#
# A path check, not a link check: core/ is compiled into every target including
# wasm, so a single #include there defeats everything above.
if grep -rniE 'luajit|lua_State|#include <lua|sol::' core/include core/src 2>/dev/null; then
  FAILED=1
  echo "FAIL: core/ mentions Lua (above). core/ is compiled into the wasm client;"
  echo "      it must not know Lua exists."
fi

if [[ "$FAILED" -ne 0 ]]; then
  echo
  echo "SOURCE LIST CHECK FAILED"
  exit 1
fi

echo "source lists ok: ${#CORE_LISTS[@]} lists agree on $REF_COUNT core sources," \
     "Lua linked in ${#LUA_IN[@]} of them, core/ is Lua-free"
