#!/usr/bin/env bash
#
# Builds the server-side Lua tests.
#
# Separate from scripts/build_native.sh on purpose: that one compiles the core
# tests against core sources ONLY, which is what lets the determinism gate run
# before any third-party toolchain is installed. A Lua test in there would make
# LuaJIT a prerequisite for checking determinism, and those two things must stay
# independent.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The repository. The engine is $ROOT; modules, the studio, the synth and
# the rest are beside it, here.
REPO="$(cd "$ROOT/.." && pwd)"
OUT="$ROOT/build/lua"
LJ="$ROOT/third_party/luajit"

if [[ ! -f "$LJ/src/libluajit.a" ]]; then
  echo "[lua-tests] LuaJIT not installed -- run scripts/setup_luajit.sh" >&2
  exit 2
fi

mkdir -p "$OUT"

FLAGS=(-std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror
       -I"$ROOT/core/include" -I"$ROOT/server/src" -I"$LJ/src")
LINK=("$LJ/src/libluajit.a" -lm)
[[ "$(uname -s)" == "Darwin" ]] || LINK+=(-ldl)

SRCS=("$ROOT/server/src/lua/vm.cpp" "$ROOT/server/src/lua/fx_binding.cpp")

# The shooting range needs the REAL world bindings and therefore the core, which
# the other Lua tests do not: they stub `dnp` with a Lua table. That is the whole
# point of it -- four bugs this phase came from a stub being more generous than
# the engine, and every one of them raised the first time a real server ran.
RANGE_SRCS=("${SRCS[@]}" "$ROOT/server/src/lua/world_binding.cpp"
            "$ROOT/server/src/lua/weapon_binding.cpp"
            "$ROOT/server/src/mobs/mob_compile.cpp"
            "$ROOT/server/src/mobs/mob_eval.cpp"
            "$ROOT/server/src/mobs/mob_runtime.cpp"
            "$ROOT/server/src/mobs/mob_binding.cpp"
            $(ls "$ROOT"/core/src/*/*.cpp))

clang++ "${FLAGS[@]}" "${SRCS[@]}" \
        "$ROOT/server/tests/test_lua_vm.cpp" "${LINK[@]}" -o "$OUT/test_lua_vm"
clang++ "${FLAGS[@]}" "${SRCS[@]}" \
        "$ROOT/server/tests/test_lua_fx.cpp" "${LINK[@]}" -o "$OUT/test_lua_fx"
clang++ "${FLAGS[@]}" "${SRCS[@]}" \
        "$ROOT/server/tests/test_lua_effects.cpp" "${LINK[@]}" -o "$OUT/test_lua_effects"
# The trigger runtime is Lua, not C++, so this one exists to RUN maps/scripts:
# it is the only place a syntax error or a broken dispatch in triggers.lua is
# caught without standing up a server, an edge and a browser.
clang++ "${FLAGS[@]}" "${SRCS[@]}" \
        "$ROOT/server/tests/test_lua_triggers.cpp" "${LINK[@]}" -o "$OUT/test_lua_triggers"
# The mob graph, as C++ compiles it. Needs the mob sources but NOT the world
# bindings: mobs.load touches no entity, so the double in the test is the few
# verbs it calls and the two real bindings are installed on top of it.
# The graph test needs core as well now: the bindings it exercises live beside
# the loop, and the loop asks the world questions. It was linking without core
# while the binding was only a compiler; that stopped being true the moment the
# same file gained dnp.mobs_tick.
clang++ "${FLAGS[@]}" "${SRCS[@]}" \
        "$ROOT/server/src/mobs/mob_compile.cpp" \
        "$ROOT/server/src/mobs/mob_eval.cpp" \
        "$ROOT/server/src/mobs/mob_runtime.cpp" \
        "$ROOT/server/src/mobs/mob_binding.cpp" \
        $(ls "$ROOT"/core/src/*/*.cpp) \
        "$ROOT/server/tests/test_mob_graph.cpp" "${LINK[@]}" -o "$OUT/test_mob_graph"
# And the loop that spends the graph, against a REAL sim::World on a real room:
# half the conditions ask the world something, and a double would be a second
# answer to all of it. Needs core, which the graph test does not.
clang++ "${FLAGS[@]}" "${SRCS[@]}" \
        "$ROOT/server/src/lua/world_binding.cpp" \
        "$ROOT/server/src/lua/weapon_binding.cpp" \
        "$ROOT/server/src/mobs/mob_compile.cpp" \
        "$ROOT/server/src/mobs/mob_eval.cpp" \
        "$ROOT/server/src/mobs/mob_runtime.cpp" \
        "$ROOT/server/src/mobs/mob_binding.cpp" \
        $(ls "$ROOT"/core/src/*/*.cpp) \
        "$ROOT/server/tests/test_mob_loop.cpp" "${LINK[@]}" -o "$OUT/test_mob_loop"
# Persistence (docs/SAVING.md): what a library saves through dnp.persist
# and gets back, against the real bindings.
clang++ "${FLAGS[@]}" "${SRCS[@]}" \
        "$ROOT/server/src/lua/world_binding.cpp" \
        "$ROOT/server/src/lua/weapon_binding.cpp" \
        "$ROOT/server/src/mobs/mob_compile.cpp" \
        "$ROOT/server/src/mobs/mob_eval.cpp" \
        "$ROOT/server/src/mobs/mob_runtime.cpp" \
        "$ROOT/server/src/mobs/mob_binding.cpp" \
        $(ls "$ROOT"/core/src/*/*.cpp) \
        "$ROOT/server/tests/test_lua_persist.cpp" "${LINK[@]}" -o "$OUT/test_lua_persist"
# Not a test: the generator that hands the trigger vocabulary to readers that
# are not Lua (the editor). It lives here because this is the only build script
# that already links LuaJIT, and because its output is checked by the suite
# above rather than by anything of its own.
clang++ "${FLAGS[@]}" "${SRCS[@]}" \
        "$ROOT/tools/vocabjson/main.cpp" "${LINK[@]}" -o "$OUT/trigger_vocab_json"
echo "[lua-tests] $OUT/test_lua_vm $OUT/test_lua_fx $OUT/test_lua_effects $OUT/test_lua_triggers"
# And the linter the editor calls to check a trigger table while somebody is
# still typing, instead of compiling a map and launching a stack to find a typo.
clang++ "${FLAGS[@]}" "${SRCS[@]}" \
        "$ROOT/tools/triggercheck/main.cpp" "${LINK[@]}" -o "$OUT/triggercheck"
clang++ "${FLAGS[@]}" "${RANGE_SRCS[@]}" \
        "$ROOT/server/tests/test_lua_shootrange.cpp" "${LINK[@]}" \
        -o "$OUT/test_lua_shootrange"
# The weapon runtime, against the same real bindings and for the same reason:
# only here can "one press, one damage event" be asked, because the two paths it
# is about -- the engine's swept collider and the script's hitscan -- exist only
# when there is a real World underneath. The flat weapon table this replaced had
# no test of any kind.
clang++ "${FLAGS[@]}" "${RANGE_SRCS[@]}" \
        "$ROOT/server/tests/test_lua_weapons.cpp" "${LINK[@]}" \
        -o "$OUT/test_lua_weapons"
# Game scripts (docs/MODULES.md): the weapons in a game VM and the monsters
# in a level's, one world under both -- the bridge between two lua_States can
# only be tested where there are two.
clang++ "${FLAGS[@]}" "${RANGE_SRCS[@]}" \
        "$ROOT/server/tests/test_lua_game.cpp" "${LINK[@]}" \
        -o "$OUT/test_lua_game"

echo "[lua-tests] $OUT/trigger_vocab_json $OUT/triggercheck"

# The server started twice in one process, for the shells that host it on a
# thread (docs/IOS.md). Built only once main.cpp exports dnp_server_run --
# the patch the server workstream applies -- so this script does not fail
# for everyone in the meantime.
if grep -q 'int dnp_server_run' "$ROOT/server/src/main.cpp"; then
  clang++ "${FLAGS[@]}" -Wno-unused-parameter "${RANGE_SRCS[@]}" \
          "$ROOT/server/src/net/link.cpp" "$ROOT/server/src/world/metamap.cpp" \
          "$ROOT/server/src/main.cpp" \
          "$ROOT/server/tests/test_server_run.cpp" "${LINK[@]}" -lpthread \
          -o "$OUT/test_server_run"
  echo "[lua-tests] $OUT/test_server_run"
fi
