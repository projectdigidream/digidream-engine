//
// The compiled mob graph IS the mob graph.
//
// server/src/mobs/ reads maps/scripts/mobs.lua's validated `defs` into flat
// arrays so a think can walk them without Lua. Everything that will later be
// decided from those arrays rests on one claim: that they say what the tables
// said. This is where that claim is checked, and it is checked against the REAL
// definitions -- maps/mobs/doom_monsters.lua, nineteen monsters and seven
// missiles -- rather than a fixture, because a fixture is a graph somebody
// wrote to be compiled and the point is the graphs people actually write.
//
// THE COMPARISON IS IN LUA, walking the two structures together. That is
// deliberate: written in C++ it would compare the compiler's output to the
// compiler's own reading of the tables, and agree with itself. Here the left
// side is the table a person authored and the right side is what came back
// across the binding, in the table's own terms -- state names, transition
// targets, condition names -- never in the indices this subsystem chose for
// itself.
//
// THE CONTROLS, and there are two, because "they match" has two ways of being
// vacuous:
//
//   it looked at something   the walk counts what it compared, and the run
//                            fails if the totals are not what a real module
//                            has in it. A comparison that silently found no
//                            states would otherwise pass.
//   it can disagree          a deliberately corrupted expectation must fail.
//                            Run at the end, against the same walk.
//
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "lua/vm.hpp"
#include "mobs/mob_runtime.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

using namespace dnp;
using namespace dnp::server::lua;

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_fails;                                                     \
            std::printf("  FAIL %s:%d   %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

static std::string g_root = ".";

// The world, as far as LOADING mobs can tell.
//
// mobs.load touches no entity and no world: it validates tables. So the double
// is the handful of verbs the file reaches for at load time, and the two real
// bindings under test are installed on top of it by the harness. `fx` and
// `vec3` are NOT doubled -- they are the real thing, because a mob's `sight` is
// an exact decimal and a stub that accepted 0.1 would make this suite agree
// with a runtime that does not exist.
static const char* const FIXTURE = R"LUA(
dnp = dnp or {}
dnp.tick = function() return 0 end
dnp.players = function() return {} end
dnp.entities = function() return {} end
dnp.random = function(a, b) return a end
)LUA";

// The walk. Left side: the authored tables. Right side: dnp.mobs_graph(name).
static const char* const COMPARE = R"LUA(
local bad, seen = {}, { kinds = 0, states = 0, transitions = 0, conditions = 0, sequence = 0 }

local function note(fmt, ...) bad[#bad + 1] = string.format(fmt, ...) end

-- mobs.lua's own defaulting, restated here rather than imported, because a
-- comparison that reused the compiler's mapping would be comparing it to
-- itself. `charge` and `stop` both steer nowhere, which is what drive() does
-- with any mode it does not name; `movement` and `hold` both leave the aim.
local MOVE = { chase = "chase", flee = "flee", strafe = "strafe",
               wander = "wander", free = "free", charge = "stop", stop = "stop" }

for name, d in pairs(mobs.defs) do
  local g = dnp.mobs_graph(name)
  if g == nil then
    note("%s: not compiled at all", name)
  else
    seen.kinds = seen.kinds + 1
    if g.hp ~= d.hp then note("%s: hp %d vs %d", name, g.hp, d.hp) end
    if g.initial ~= d.initial then
      note("%s: initial '%s' vs '%s'", name, tostring(g.initial), tostring(d.initial))
    end
    if (g.flying and true or false) ~= (d.flying and true or false) then
      note("%s: flying differs", name)
    end

    -- Every state on each side is on the other. Both directions: a compiler
    -- that dropped one and a compiler that invented one are different bugs.
    local n_lua, n_cpp = 0, 0
    for _ in pairs(d.states) do n_lua = n_lua + 1 end
    for _ in pairs(g.states) do n_cpp = n_cpp + 1 end
    if n_lua ~= n_cpp then note("%s: %d states vs %d", name, n_cpp, n_lua) end

    for sname, st in pairs(d.states) do
      local gs = g.states[sname]
      if gs == nil then
        note("%s.%s: missing", name, sname)
      else
        seen.states = seen.states + 1
        if gs.anim ~= (st.anim or -1) then
          note("%s.%s: anim %d vs %s", name, sname, gs.anim, tostring(st.anim))
        end
        if gs.ticks ~= (st.ticks or -1) then
          note("%s.%s: ticks %d vs %s", name, sname, gs.ticks, tostring(st.ticks))
        end
        if gs.next ~= st.next then
          note("%s.%s: next '%s' vs '%s'", name, sname, tostring(gs.next), tostring(st.next))
        end
        local want_move = MOVE[st.move or "stop"]
        if gs.move ~= want_move then
          note("%s.%s: move '%s' vs '%s'", name, sname, gs.move, tostring(want_move))
        end
        local want_face = ((st.face or "target") == "target") and "target" or "hold"
        if gs.face ~= want_face then
          note("%s.%s: face '%s' vs '%s'", name, sname, gs.face, want_face)
        end
        local want_solid = (st.solid == nil) and -1 or (st.solid and 1 or 0)
        if gs.solid ~= want_solid then
          note("%s.%s: solid %d vs %d", name, sname, gs.solid, want_solid)
        end

        local tl = st.transitions or {}
        if #gs.transitions ~= #tl then
          note("%s.%s: %d transitions vs %d", name, sname, #gs.transitions, #tl)
        end
        for i = 1, math.min(#gs.transitions, #tl) do
          seen.transitions = seen.transitions + 1
          local gt, lt = gs.transitions[i], tl[i]
          if gt.to ~= lt.to then
            note("%s.%s[%d]: to '%s' vs '%s'", name, sname, i, tostring(gt.to), tostring(lt.to))
          end
          local wl = lt.when or {}
          if #gt.when ~= #wl then
            note("%s.%s[%d]: %d conditions vs %d", name, sname, i, #gt.when, #wl)
          end
          for j = 1, math.min(#gt.when, #wl) do
            seen.conditions = seen.conditions + 1
            local gc, lc = gt.when[j], wl[j]
            -- A condition C++ does not implement is reported as
            -- "unsupported" and the KIND is marked; that is not a mismatch,
            -- it is the documented outcome, so it is checked separately.
            if gc.cond ~= "unsupported" and gc.cond ~= lc.cond then
              note("%s.%s[%d].%d: cond '%s' vs '%s'", name, sname, i, j, gc.cond, lc.cond)
            end
            if gc.cond == "unsupported" and g.supported then
              note("%s: has an unsupported condition and was not marked", name)
            end
            if (gc.negate ~= false) ~= (lc.negate == true) then
              note("%s.%s[%d].%d: negate differs", name, sname, i, j)
            end
            -- EXACT, through the raw, because `distance` is an fx and the
            -- whole reason that type exists is that a double would round it.
            local want_raw = lc.distance ~= nil and lc.distance.raw or "0"
            if gc.f_raw ~= want_raw then
              note("%s.%s[%d].%d: distance raw %s vs %s",
                   name, sname, i, j, gc.f_raw, want_raw)
            end
            local want_i = lc.percent or lc.ticks or lc.value or lc.arc or lc.within or 0
            if gc.cond ~= "unsupported" and gc.i ~= want_i then
              note("%s.%s[%d].%d: arg %d vs %d", name, sname, i, j, gc.i, want_i)
            end
            if lc.var ~= nil and gc.var < 0 then
              note("%s.%s[%d].%d: var '%s' got no slot", name, sname, i, j, lc.var)
            end
          end
        end

        local sl = st.sequence or {}
        if #gs.sequence ~= #sl then
          note("%s.%s: %d sequence entries vs %d", name, sname, #gs.sequence, #sl)
        end
        for i = 1, math.min(#gs.sequence, #sl) do
          seen.sequence = seen.sequence + 1
          local ge, le = gs.sequence[i], sl[i]
          if ge.at ~= le.at then
            note("%s.%s seq %d: at %d vs %d", name, sname, i, ge.at, le.at)
          end
          -- The index INTO LUA is what makes an action still runnable there.
          -- Off by one and the mob fires the wrong action at the right tick,
          -- which is the sort of thing that looks like a gameplay change.
          if ge.lua_index ~= i then
            note("%s.%s seq %d: lua_index %d", name, sname, i, ge.lua_index)
          end
          if le.action == "goto" and ge.goto_state ~= le.state then
            note("%s.%s seq %d: goto '%s' vs '%s'", name, sname, i,
                 tostring(ge.goto_state), tostring(le.state))
          end
        end
      end
    end
  end
end

MISMATCHES = bad
SEEN = seen
)LUA";

static bool run(Vm& vm, const char* chunk, const char* name) {
    lua_State* L = vm.state();
    vm.begin_tick();
    if (luaL_loadbuffer(L, chunk, std::strlen(chunk), name) != 0 ||
        lua_pcall(L, 0, 0, 0) != 0) {
        std::printf("  FAIL %s: %s\n", name, lua_tostring(L, -1));
        lua_pop(L, 1);
        ++g_fails;
        ++g_checks;
        return false;
    }
    return true;
}

static bool run_file(Vm& vm, const std::string& path) {
    vm.begin_tick();
    if (!vm.run_file(path.c_str(), path.c_str())) {
        std::printf("  FAIL loading %s: %s\n", path.c_str(), vm.last_error());
        ++g_fails;
        ++g_checks;
        return false;
    }
    return true;
}

static lua_Number number_field(lua_State* L, const char* table, const char* key) {
    lua_getglobal(L, table);
    lua_getfield(L, -1, key);
    const lua_Number v = lua_tonumber(L, -1);
    lua_pop(L, 2);
    return v;
}

int main(int argc, char** argv) {
    if (argc > 1) g_root = argv[1];
    std::printf("== the compiled mob graph is the mob graph ==\n");

    Vm vm;
    VmConfig cfg;
    // The real definitions are large and the loader builds a lot of tables.
    cfg.budget_per_call = 200000000;
    cfg.budget_per_tick = 200000000;
    if (!vm.open(cfg)) {
        std::printf("  FAIL vm.open: %s\n", vm.last_error());
        return 1;
    }

    // The double first, then the two real bindings on top of the same table.
    if (!run(vm, FIXTURE, "fixture")) return 1;
    {
        lua_State* L = vm.state();
        lua_getglobal(L, "dnp");
        dnp::server::mobs::open_mobs(L);
        lua_pop(L, 1);
    }

    if (!run_file(vm, g_root + "/maps/scripts/mob_vocab.lua")) return 1;
    if (!run_file(vm, g_root + "/maps/scripts/mobs.lua")) return 1;
    // doom_monsters.lua calls mobs.load itself -- a map declares it as a script
    // and that is the whole of the wiring -- so loading the file IS the load.
    if (!run_file(vm, g_root + "/maps/mobs/doom_monsters.lua")) return 1;

    lua_State* L = vm.state();

    if (!run(vm, "PROBLEMS = mobs.problems", "problems")) return 1;
    {
        lua_getglobal(L, "PROBLEMS");
        const int n = static_cast<int>(lua_objlen(L, -1));
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            std::printf("  loader: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        CHECK(n == 0);
    }

    // IT COMPILED SOMETHING. Without this the walk below can pass by finding
    // nothing at all to walk.
    const int compiled = static_cast<int>(number_field(L, "mobs", "compiled"));
    const int on_lua = static_cast<int>(number_field(L, "mobs", "on_lua_path"));
    std::printf("  %d kinds compiled, %d on the Lua path\n", compiled, on_lua);

    // Against what mobs.defs ACTUALLY holds, counted in Lua, rather than
    // against a number written here. A literal would be a second place the
    // module's size is recorded, and it would start failing the day somebody
    // adds a monster -- which is not a regression.
    if (!run(vm, "N_DEFS = 0 for _ in pairs(mobs.defs) do N_DEFS = N_DEFS + 1 end",
             "count")) return 1;
    lua_getglobal(L, "N_DEFS");
    const int n_defs = static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    std::printf("  mobs.defs holds %d\n", n_defs);
    CHECK(compiled + on_lua == n_defs);
    // And it is a real module, not an empty one -- the floor that stops this
    // passing against nothing.
    CHECK(n_defs >= 15);

    if (!run(vm, COMPARE, "compare")) return 1;

    lua_getglobal(L, "MISMATCHES");
    const int nbad = static_cast<int>(lua_objlen(L, -1));
    for (int i = 1; i <= nbad && i <= 40; ++i) {
        lua_rawgeti(L, -1, i);
        std::printf("  MISMATCH %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    CHECK(nbad == 0);

    // AND IT LOOKED AT SOMETHING. Nineteen monsters with several states each,
    // and the thresholds are floors rather than exact counts so that adding a
    // monster does not fail the suite -- but losing most of them does.
    const int states = static_cast<int>(number_field(L, "SEEN", "states"));
    const int trans  = static_cast<int>(number_field(L, "SEEN", "transitions"));
    const int conds  = static_cast<int>(number_field(L, "SEEN", "conditions"));
    const int seq    = static_cast<int>(number_field(L, "SEEN", "sequence"));
    std::printf("  compared %d states, %d transitions, %d conditions, %d sequence entries\n",
                states, trans, conds, seq);
    CHECK(states >= 80);
    CHECK(trans >= 40);
    CHECK(conds >= 40);
    CHECK(seq >= 40);

    // THE CONTROL: the same walk, against a graph that has been altered. If a
    // corrupted expectation still passes, the walk is not comparing anything.
    //
    // The alteration is a single condition's distance, deep inside one
    // monster's table -- not a whole state removed, because a coarse break
    // would pass a walk that only counted things.
    const char* const BREAK = R"LUA(
      BROKE = false
      for _, d in pairs(mobs.defs) do
        for _, st in pairs(d.states) do
          for _, t in ipairs(st.transitions or {}) do
            for _, c in ipairs(t.when or {}) do
              if c.distance ~= nil and not BROKE then
                c.distance = c.distance + fx.int(1)
                BROKE = true
              end
            end
          end
        end
      end
    )LUA";
    if (run(vm, BREAK, "break")) {
        lua_getglobal(L, "BROKE");
        const bool broke = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        CHECK(broke);                       // there WAS a distance to perturb
        if (broke && run(vm, COMPARE, "compare-after-break")) {
            lua_getglobal(L, "MISMATCHES");
            const int n2 = static_cast<int>(lua_objlen(L, -1));
            lua_pop(L, 1);
            std::printf("  control: one altered distance produces %d mismatch(es)\n", n2);
            CHECK(n2 > 0);
        }
    }

    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
