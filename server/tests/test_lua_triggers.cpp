//
// The trigger runtime: maps/scripts/triggers.lua, as an interpreter.
//
// What is under test is the INTERPRETER, not the bindings it calls: dispatch,
// conditions, argument validation, tick deadlines, enable/disable and the
// bounded `run` nesting. So the world is a test double written in Lua -- a
// handful of tables with the same verbs the real handles answer -- and every
// action is observed by what it recorded rather than by what moved.
//
// That split is deliberate. The bindings are already covered end to end by
// server/edge/test/triggers.test.js, which drives a real server on a real map
// and asserts a real ceiling moved. What that test cannot do cheaply is
// enumerate forty ways to write a broken trigger table, and this one cannot
// prove a door opens. Both claims need making.
//
// `fx` and `vec3` are NOT doubled. They are the real binding, because an
// author writes a door's speed as an exact decimal and the whole point of that
// type is to refuse the values a double cannot hold -- a stub that accepted
// 0.1 would make this suite agree with a runtime that does not exist.
//
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "lua/vm.hpp"

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

#define CHECK_MSG(cond, msg)                                                \
    do {                                                                    \
        ++g_checks;                                                         \
        if (!(cond)) {                                                      \
            ++g_fails;                                                      \
            std::printf("  FAIL %s:%d   %s\n", __FILE__, __LINE__, (msg));  \
        }                                                                   \
    } while (0)

static std::string g_root = ".";

// ---------------------------------------------------------------- the double
//
// The world, as far as a trigger can tell. Sectors, entities and sides with
// the same verbs the real handles carry, plus a LOG every mutation appends to
// -- so an assertion is about what the runtime ASKED FOR, which is the only
// part of an action this file is entitled to claim anything about.
static const char* const FIXTURE = R"LUA(
LOG = {}
TICK = 0

function note(s) LOG[#LOG + 1] = s end
function logged(s)
  for i = 1, #LOG do if LOG[i] == s then return true end end
  return false
end
function log_count(s)
  local n = 0
  for i = 1, #LOG do if LOG[i] == s then n = n + 1 end end
  return n
end
function reset_log() LOG = {} end

-- Sectors ----------------------------------------------------------------
local sector = {}
sector.__index = sector
sector.__eq = function(a, b) return a.i == b.i end
function sector:index() return self.i end
function sector:tag() return self.t end
function sector:floor() return self.f + (self.off and self.off.z or fx.int(0)) end
function sector:ceil() return self.c + (self.off and self.off.z or fx.int(0)) end
function sector:offset() return self.off end
function sector:neighbors()
  local out = {}
  for _, n in ipairs(self.nb) do out[#out + 1] = SECTORS[n] end
  return out
end
function sector:move_parametric(from, to, n, loop, surface, spin, pivot)
  if self.refuse then return false end
  note(("move %d %s to=%s ticks=%d %s"):format(self.i, surface, tostring(to.z), n, loop))
  MOVES[#MOVES + 1] = { sector = self.i, surface = surface, z = to.z,
                        x = to.x, y = to.y, ticks = n, loop = loop,
                        spin = spin, pivot = pivot }
  return true
end
function sector:set_offset(v)
  self.off = v
  note(("offset %d %s"):format(self.i, tostring(v.z)))
  return true
end

SECTORS = {}
MOVES = {}
function add_sector(i, tag, floor, ceil, nb)
  SECTORS[i] = setmetatable({ i = i, t = tag, f = fx.int(floor), c = fx.int(ceil),
                              nb = nb or {}, off = nil }, sector)
  return SECTORS[i]
end

-- Entities ---------------------------------------------------------------
local ent = {}
ent.__index = ent
ent.__eq = function(a, b) return a.i == b.i end
function ent:index() return self.i end
function ent:kind() return self.k end
function ent:name() return self.n end
-- The sector's NUMBER, as the engine's binding gives it (-1 for a body in
-- another level). It gave the sector here, more generously than the engine,
-- and so hid a condition that indexed the number as a sector and threw.
function ent:sector() return self.s end
function ent:hp() return self.health end
function ent:set_hp(v) self.health = v; note(("hp %d %d"):format(self.i, v)) end
function ent:set_solid(b) note(("solid %d %s"):format(self.i, tostring(b))) end
function ent:set_animation(n) note(("anim %d %d"):format(self.i, n)) end
function ent:despawn() note(("despawn %d"):format(self.i)) return true end
function ent:teleport(v) note(("teleport %d %s"):format(self.i, tostring(v.z))) end
function ent:add_effect(p, op, v, t)
  note(("effect %d %s %s"):format(self.i, p, op)); return 1
end
function ent:change_map(l) note(("changemap %d"):format(self.i)); return true end

ENTS = {}
function add_entity(i, kind, name, sec)
  ENTS[i] = setmetatable({ i = i, k = kind, n = name, s = sec, health = 100 }, ent)
  return ENTS[i]
end
function ENT(i) return ENTS[i] end
function SEC(i) return SECTORS[i] end

-- Sides ------------------------------------------------------------------
SIDES = {}
function add_side(i, neighbor) SIDES[i] = { neighbor = SECTORS[neighbor] } end

-- The binding ------------------------------------------------------------
dnp = {
  tick = function() return TICK end,
  sector = function(i) return SECTORS[i] end,
  sectors = function()
    local out = {}
    for _, s in pairs(SECTORS) do out[#out + 1] = s end
    table.sort(out, function(a, b) return a.i < b.i end)
    return out
  end,
  side = function(i) return SIDES[i] end,
  prop = function(i) return nil end,
  props = function() return {} end,
  spawn_prop = function(n, p) note("spawn_prop " .. n); return {} end,
  spawn_enemy = function(n, p) note("spawn_enemy " .. n); return {} end,
  ui_open = function(e, panel) note("ui_open " .. panel.title); return 7 end,
  ui_close = function(e, id) note(("ui_close %d"):format(id)) end,
  camera = function(e, t)
    for k, v in pairs(t) do note(("camera %s %s"):format(k, tostring(v))) end
  end,
  music = function(e, cue, fade, at)
    note(("music %s %s %s %s"):format(e and tostring(e.i) or "all", cue, tostring(fade), tostring(at)))
  end,
  vfx = function(kind, pos, dir, param) note(("vfx %d"):format(kind)); return true end,
  -- The engine's own 2D length, faked with a double root. The runtime uses it
  -- to turn a sideways move's travel into a duration, so a stub without it
  -- raises the moment a trigger names `dx`.
  length = function(v)
    local x, y = tonumber(tostring(v.x)), tonumber(tostring(v.y))
    return fx.parse(("%.6f"):format(math.sqrt(x * x + y * y)))
  end,
}
)LUA";

// A world with a door in it, shaped like the one in maps/doors.map: a room, a
// door sector shut against its own floor, and a corridor whose ceiling is what
// the door opens up to.
static const char* const WORLD = R"LUA(
add_sector(0, 1, 0, 8, {1})
add_sector(1, 2, 0, 0, {0, 2})     -- the door: ceiling resting on its floor
add_sector(2, 3, 0, 2, {1})
add_sector(3, 5, 3, 8, {2})        -- a lift, parked up
-- Sides, and what is behind each. Every side a trigger names has to exist here
-- or the load-time geometry guard cannot resolve it -- which is the point of
-- the guard, and is also why this list is longer than the door.
add_side(2, 1)                     -- the side you press to work the door
add_side(3)                        -- a plain wall
add_side(9)                        -- another
add_side(11, 2)                    -- a trip-wire, eastbound
add_side(21, 2)                    -- and westbound
add_entity(0, 0, nil, 0)           -- a player: kind 0, nobody placed it
add_entity(1, 4, "imp", 0)
)LUA";

// -------------------------------------------------------------------- driving

struct Fixture {
    Vm vm;

    bool open(Trust trust = Trust::Full) {
        VmConfig cfg;
        cfg.trust = trust;
        if (!vm.open(cfg)) return false;
        if (!exec(FIXTURE, "=fixture")) return false;
        if (!exec(WORLD, "=world")) return false;
        const std::string vocab = g_root + "/maps/scripts/trigger_vocab.lua";
        const std::string rt    = g_root + "/maps/scripts/triggers.lua";
        if (!vm.run_file(vocab.c_str(), "trigger_vocab.lua")) {
            std::printf("  FAIL loading %s: %s\n", vocab.c_str(), vm.last_error());
            return false;
        }
        if (!vm.run_file(rt.c_str(), "triggers.lua")) {
            std::printf("  FAIL loading %s: %s\n", rt.c_str(), vm.last_error());
            return false;
        }
        return true;
    }

    // Run a chunk. Errors are printed, because a fixture that failed silently
    // makes every later assertion measure nothing.
    bool exec(const char* src, const char* name = "=probe") {
        lua_State* L = vm.state();
        vm.begin_tick();
        if (luaL_loadbuffer(L, src, std::strlen(src), name) != 0) {
            std::printf("  LUA  %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
            return false;
        }
        if (lua_pcall(L, 0, 0, 0) != 0) {
            std::printf("  LUA  %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
            return false;
        }
        return true;
    }

    // True if the expression is true. A Lua error counts as false and is
    // printed -- a typo in an assertion must not read as a failing assertion.
    bool truth(const char* expr) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "if not (%s) then error('false') end", expr);
        lua_State* L = vm.state();
        vm.begin_tick();
        if (luaL_loadbuffer(L, buf, std::strlen(buf), "=check") != 0) {
            std::printf("  LUA  %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
            return false;
        }
        if (lua_pcall(L, 0, 0, 0) != 0) {
            const char* m = lua_tostring(L, -1);
            if (m && std::strstr(m, "false") == nullptr) std::printf("  LUA  %s\n", m);
            lua_pop(L, 1);
            return false;
        }
        return true;
    }

    // Push one hook argument by evaluating a Lua expression -- which is how an
    // entity or a sector double gets onto the stack without this file knowing
    // anything about its representation.
    void push_arg(const char* expr) {
        lua_State* L = vm.state();
        char buf[256];
        std::snprintf(buf, sizeof(buf), "return %s", expr);
        if (luaL_loadbuffer(L, buf, std::strlen(buf), "=arg") != 0 ||
            lua_pcall(L, 0, 1, 0) != 0) {
            std::printf("  LUA  building argument '%s': %s\n", expr, lua_tostring(L, -1));
            lua_pop(L, 1);
            lua_pushnil(L);
        }
    }

    void fire(const char* hook, u32 tick, const std::vector<std::string>& args) {
        for (u32 h = 0; h < vm.hook_count(hook); ++h) {
            if (!vm.begin_hook_at(hook, h)) continue;
            for (const std::string& a : args) push_arg(a.c_str());
            vm.end_hook(hook, tick, static_cast<int>(args.size()));
        }
    }

    // One tick: advance the double's clock and call on_tick, which is where
    // deadlines are drained.
    void tick(u32 t) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "TICK = %u", t);
        exec(buf);
        fire("on_tick", t, {});
    }
};

// ------------------------------------------------------------------ the tests

static void test_the_vocabulary_and_the_runtime_agree() {
    std::printf("triggers: every action in the vocabulary has an implementation\n");

    Fixture f;
    CHECK(f.open());
    CHECK(f.exec("assert(triggers.load{ name='t', triggers = {"
                 "  { name='a', event='on_tick', actions={ {a='log', text='hi'} } } } })"));

    // CONTROL. Take an action out of the vocabulary and the runtime must refuse
    // to load anything at all -- otherwise the check is a comment.
    //
    // The direction matters: this removes a VOCABULARY entry while the handler
    // stays, which is the case an editor would hit (it offers what the table
    // says) and the one a naive "does every name resolve" check misses.
    Fixture g;
    CHECK(g.open());
    CHECK(g.exec("table.remove(trigger_vocab.actions, 1)"));
    CHECK_MSG(g.truth("triggers.load{ name='t', triggers = {"
                      "  { name='a', event='on_tick', actions={ {a='log', text='hi'} } } } }"
                      " == false"),
              "a vocabulary missing an action the runtime implements must refuse to load");
}

static void test_a_broken_table_loads_nothing() {
    std::printf("triggers: a table with a mistake in it loads nothing, and says which\n");

    // FAIL CLOSED. A map that half works is harder to diagnose than one that
    // refuses and names the line: "the switch does nothing" is what both look
    // like from inside the game, and only one of them says why.
    const char* const broken[] = {
        // an action that does not exist
        "{ name='a', event='on_interact', actions={ {a='open_door', sector=1} } }",
        // an event the engine does not report
        "{ name='a', event='on_footstep', actions={ {a='log', text='x'} } }",
        // a condition that does not exist
        "{ name='a', event='on_tick', conditions={ {c='is_tuesday'} },"
        "  actions={ {a='log', text='x'} } }",
        // a required argument missing
        "{ name='a', event='on_tick', actions={ {a='counter_add'} } }",
        // an argument the action does not have -- almost always a typo, and a
        // silently ignored one is a trigger that looks configured and is not
        "{ name='a', event='on_tick', actions={ {a='log', text='x', colour='red'} } }",
        // an enum outside its values
        "{ name='a', event='on_tick', actions={"
        "   {a='sector_move', sector=1, surface='sideways', speed=1} } }",
        // both halves of a one_of group
        "{ name='a', event='on_tick', actions={"
        "   {a='sector_move', sector=1, tag=2, surface='floor', speed=1} } }",
        // neither half of a one_of group
        "{ name='a', event='on_tick', actions={"
        "   {a='sector_move', surface='floor', speed=1} } }",
        // a trigger reference that does not resolve
        "{ name='a', event='on_tick', actions={ {a='run', trigger='nowhere'} } }",
        // two triggers with the same name: `run` would silently pick one
        "{ name='a', event='on_tick', actions={ {a='log', text='x'} } },"
        "{ name='a', event='on_tick', actions={ {a='log', text='y'} } }",
        // no actions at all
        "{ name='a', event='on_tick', conditions={ {c='tick_at_least', tick=1} } }",
        // a condition reading something its event does not carry
        "{ name='a', event='on_crush', conditions={ {c='side_is', side=2, expect_neighbour=1} },"
        "  actions={ {a='log', text='x'} } }",
        // a double where an exact decimal was meant
        "{ name='a', event='on_tick', actions={"
        "   {a='sector_move', sector=1, surface='floor', speed=0.25} } }",
        // a fraction that is not one
        "{ name='a', event='on_tick', actions={"
        "   {a='sector_move', sector=1, surface='floor', speed={1}} } }",
        // ...and one that divides by zero
        "{ name='a', event='on_tick', actions={"
        "   {a='sector_move', sector=1, surface='floor', speed={1, 0}} } }",
        // custom code that does not compile. Caught at LOAD, which is the
        // whole point: the alternative is a syntax error surfacing the first
        // time somebody presses the switch.
        "{ name='a', event='on_tick', actions={ {a='lua', code='if then end'} } }",
        // ...and neither form of it given
        "{ name='a', event='on_tick', actions={ {a='lua'} } }",
        // ...and both
        "{ name='a', event='on_tick', actions={"
        "   {a='lua', code='return true', fn=function() return true end} } }",
        // a property holding something a condition could not compare
        "{ name='a', event='on_tick', actions={"
        "   {a='set_property', name='x', value={1,2}} } }",
        // a typo in the TRIGGER's own field name. Legal Lua, and silently a
        // trigger with no event -- which looks from inside the game exactly
        // like one that never fires.
        "{ name='a', evnt='on_tick', actions={ {a='log', text='x'} } }",
    };

    for (const char* t : broken) {
        Fixture f;
        if (!f.open()) { CHECK(false); continue; }
        std::string src = "BAD = triggers.load{ name='t', triggers = { ";
        src += t;
        src += " } }";
        f.exec(src.c_str());
        CHECK_MSG(f.truth("BAD == false"), t);
        // And nothing of it is live: a table that failed validation must not
        // have left half its triggers registered.
        CHECK_MSG(f.truth("triggers.is_enabled('a') == false"), t);
    }

    // CONTROL: the same shapes, correct, must load. Without this the assertions
    // above are satisfied by a runtime that refuses everything.
    const char* const good[] = {
        "{ name='a', event='on_interact', actions={ {a='log', text='x'} } }",
        "{ name='a', event='on_tick', actions={ {a='counter_add', counter='n'} } }",
        "{ name='a', event='on_tick', actions={"
        "   {a='sector_move', sector=1, surface='floor', speed='0.25'} } }",
        "{ name='a', event='on_tick', actions={"
        "   {a='sector_move', tag=2, surface='floor', speed=1} } }",
        "{ name='a', event='on_tick', actions={"
        "   {a='sector_move', sector=1, surface='ceiling', speed={1, 4},"
        "    to='lowest_neighbour_ceiling', offset={-4, 48}} } }",
        "{ name='a', event='on_crush', conditions={ {c='crush_kind_is', kind='vertical'} },"
        "  actions={ {a='log', text='x'} } }",
        "{ name='a', event='on_tick', actions={ {a='lua', code='return true'} } }",
        "{ name='a', event='on_tick', actions={"
        "   {a='lua', fn=function() return true end} } }",
        "{ name='a', event='on_tick', actions={"
        "   {a='set_property', name='door', value='open'} } }",
    };
    for (const char* t : good) {
        Fixture f;
        if (!f.open()) { CHECK(false); continue; }
        std::string src = "OK = triggers.load{ name='t', triggers = { ";
        src += t;
        src += " } }";
        f.exec(src.c_str());
        CHECK_MSG(f.truth("OK == true"), t);
    }
}

// A body's sector, by tag: entity:sector() is a number, and the condition has
// to look the sector up rather than index the number.
static void test_entity_in_sector_tag() {
    std::printf("triggers: entity_in_sector_tag finds the body's sector by its number\n");
    Fixture f;
    CHECK(f.open());
    CHECK(f.exec(R"LUA(
add_entity(5, 0, nil, -1)          -- a player in another level
triggers.load{ name = 'tagged', triggers = {
  { name = 'in tag 1', event = 'on_interact',
    conditions = { { c = 'entity_in_sector_tag', tag = 1 } },
    actions = { { a = 'counter_add', counter = 'one' } } },
  { name = 'in tag 3', event = 'on_interact',
    conditions = { { c = 'entity_in_sector_tag', tag = 3 } },
    actions = { { a = 'counter_add', counter = 'three' } } },
} }
)LUA"));
    // The player stands in sector 0, whose tag is 1.
    f.fire("on_interact", 1, {"ENT(0)", "2", "SEC(0)"});
    CHECK(f.truth("triggers.counter('one') == 1"));
    CHECK(f.truth("triggers.counter('three') == 0"));   // CONTROL: the other tag does not match
    // Standing in no sector of this level: no match, and no error.
    f.fire("on_interact", 2, {"ENT(5)", "2", "SEC(0)"});
    CHECK(f.truth("triggers.counter('one') == 1"));
}

static void test_conditions_select() {
    std::printf("triggers: conditions decide which trigger an event reaches\n");

    Fixture f;
    CHECK(f.open());
    CHECK(f.exec(R"LUA(
triggers.load{ name = 'sel', triggers = {
  { name = 'the door switch', event = 'on_interact',
    conditions = { { c = 'side_is', side = 2, expect_neighbour = 1 } },
    actions = { { a = 'counter_add', counter = 'door' } } },
  { name = 'anything at all', event = 'on_interact',
    actions = { { a = 'counter_add', counter = 'any' } } },
  { name = 'not the door switch', event = 'on_interact',
    conditions = { { c = 'side_is', side = 2, expect_neighbour = 1, negate = true } },
    actions = { { a = 'counter_add', counter = 'other' } } },
  { name = 'a player pressed it', event = 'on_interact',
    conditions = { { c = 'entity_is_player' } },
    actions = { { a = 'counter_add', counter = 'player' } } },
} }
)LUA"));

    f.fire("on_interact", 1, {"ENT(0)", "2", "SEC(0)"});
    CHECK(f.truth("triggers.counter('door') == 1"));
    CHECK(f.truth("triggers.counter('other') == 0"));
    CHECK(f.truth("triggers.counter('player') == 1"));

    f.fire("on_interact", 2, {"ENT(1)", "3", "SEC(0)"});
    CHECK(f.truth("triggers.counter('door') == 1"));
    CHECK(f.truth("triggers.counter('other') == 1"));
    // The imp has a catalogue kind, so it is not a player.
    CHECK(f.truth("triggers.counter('player') == 1"));

    // CONTROL: the unconditional trigger saw both, so the two above did not
    // simply fail to be dispatched to.
    CHECK_MSG(f.truth("triggers.counter('any') == 2"),
              "an unconditional trigger must see every event, or the selection "
              "above is indistinguishable from no dispatch at all");
}

static void test_a_door_is_a_sector_and_a_side() {
    std::printf("triggers: 'behind' finds the sector on the other side of the switch\n");

    Fixture f;
    CHECK(f.open());
    CHECK(f.exec(R"LUA(
triggers.load{ name = 'door', triggers = {
  { name = 'open the door', event = 'on_interact',
    conditions = { { c = 'side_is', side = 2, expect_neighbour = 1 },
                   { c = 'sector_at_rest', sector = 'behind' } },
    actions = { { a = 'sector_move', sector = 'behind', surface = 'ceiling',
                  to = 'lowest_neighbour_ceiling', offset = '-0.0834',
                  speed = '0.25' } } },
} }
)LUA"));

    f.fire("on_interact", 0, {"ENT(0)", "2", "SEC(0)"});

    // Sector 1's neighbours are 0 (ceiling 8) and 2 (ceiling 2), so the lowest
    // neighbouring ceiling is 2 and the door opens to 2 - 0.0834. Its own base
    // ceiling is 0, so that is also the offset it travels to.
    CHECK(f.truth("#MOVES == 1"));
    CHECK(f.truth("MOVES[1].sector == 1"));
    CHECK(f.truth("MOVES[1].surface == 'ceiling'"));
    CHECK(f.truth("MOVES[1].z == fx.int(2) - fx.parse('0.0834')"));
    CHECK(f.truth("MOVES[1].loop == 'once'"));
    // 1.9166 units at 0.25 a tick is 7 ticks, floored, and never zero.
    CHECK(f.truth("MOVES[1].ticks == 7"));
}

// A PLATFORM THAT GOES SIDEWAYS, from the vocabulary rather than from raw Lua.
//
// The engine has carried riders and shoved bodies horizontally since sector
// motion existed -- core/tests/test_sectormotion.cpp measures both -- and this
// vocabulary could only ask for heights, so every sliding platform in the
// repository was a hand-written script.
static void test_a_sector_can_be_moved_sideways() {
    std::printf("triggers: a platform can be sent sideways, not only up\n");

    Fixture f;
    CHECK(f.open());
    CHECK(f.exec(R"LUA(
triggers.load{ name = 'slide', triggers = {
  { name = 'go', event = 'on_cross',
    actions = { { a = 'sector_move', sector = 1, surface = 'both',
                  dx = 6, speed = '0.5' } } },
  { name = 'diagonal', event = 'on_interact',
    actions = { { a = 'sector_move', sector = 1, surface = 'both',
                  dx = 3, dy = 4, speed = '0.5' } } },
} }
)LUA"));

    f.fire("on_cross", 0, {"ENT(0)", "11", "SEC(0)"});
    CHECK(f.truth("#MOVES == 1"));
    CHECK(f.truth("MOVES[1].x == fx.int(6)"));
    CHECK(f.truth("MOVES[1].y == fx.int(0)"));
    // AND IT DOES NOT ALSO COME HOME. `to` and `height` are absent, and
    // resolve_height answers "the base floor" for that -- which is right for a
    // lift with nothing else said and wrong here. A platform already raised
    // would otherwise sink as it travelled.
    CHECK(f.truth("MOVES[1].z == fx.int(0)"));
    // Six units at half a unit a tick.
    CHECK(f.truth("MOVES[1].ticks == 12"));

    // ALONG THE LINE ACTUALLY TRAVELLED. 3 and 4 is 5, not 7 and not 4 -- so
    // `speed` means the same thing for a lift, a slider and a diagonal.
    // A version that added the axes would say 14 ticks and one that took the
    // larger would say 8.
    //
    // AFTER the first slide has arrived. A sector still in flight refuses a
    // second start -- the rule the door test above is about -- and a diagonal
    // measured against a refusal reports no move at all, not a wrong duration.
    f.exec("TICK = 20");
    f.fire("on_interact", 20, {"ENT(0)", "2", "SEC(0)"});
    CHECK(f.truth("#MOVES == 2"));
    CHECK(f.truth("MOVES[2].x == fx.int(3)"));
    CHECK(f.truth("MOVES[2].y == fx.int(4)"));
    CHECK(f.truth("MOVES[2].ticks == 10"));
}

// AND IT CAN TURN. A rotating platform is a harder thing than a sliding one --
// it stops being the SHAPE the map says it is -- and core/tests/test_sectormotion
// measures what that does to riders. This is about the vocabulary reaching it.
static void test_a_sector_can_be_spun() {
    std::printf("triggers: a platform can be spun, about a pivot or its own middle\n");

    Fixture f;
    CHECK(f.open());
    CHECK(f.exec(R"LUA(
triggers.load{ name = 'spin', triggers = {
  { name = 'turn', event = 'on_cross',
    actions = { { a = 'sector_move', sector = 1, surface = 'both',
                  spin = 90, spin_speed = 3, speed = '0.5' } } },
  { name = 'about a point', event = 'on_interact',
    actions = { { a = 'sector_move', sector = 1, surface = 'both',
                  spin = 90, spin_speed = 3, speed = '0.5',
                  pivot_x = 4, pivot_y = 7 } } },
} }
)LUA"));

    f.fire("on_cross", 0, {"ENT(0)", "11", "SEC(0)"});
    CHECK(f.truth("#MOVES == 1"));
    CHECK(f.truth("MOVES[1].spin == fx.int(90)"));
    // Ninety degrees at three a tick. The TURN sets the duration here, because
    // there is no travel at all -- and `speed` alone would have said one tick,
    // which is a platform that teleports round.
    CHECK(f.truth("MOVES[1].ticks == 30"));
    // No pivot named, so the runtime passes none and the binding falls back to
    // the mean of the sector's own corners. Passing (0,0) instead would spin
    // every sector about the map origin, which is a very different picture.
    CHECK(f.truth("MOVES[1].pivot == nil"));

    f.exec("TICK = 40");
    f.fire("on_interact", 40, {"ENT(0)", "2", "SEC(0)"});
    CHECK(f.truth("#MOVES == 2"));
    CHECK(f.truth("MOVES[2].pivot ~= nil"));
    CHECK(f.truth("MOVES[2].pivot.x == fx.int(4)"));
    CHECK(f.truth("MOVES[2].pivot.y == fx.int(7)"));
}

// CONTROL: a move that names no dx or dy is the vertical one it always was.
//
// Without this the two assertions above are satisfied by a runtime that puts
// the travel somewhere in every move -- and every door in every converted level
// would have started sliding.
static void test_a_move_with_no_travel_is_still_vertical() {
    std::printf("triggers: CONTROL -- a move naming no travel stays where it is\n");

    Fixture f;
    CHECK(f.open());
    CHECK(f.exec(R"LUA(
triggers.load{ name = 'lift', triggers = {
  { name = 'up', event = 'on_cross',
    actions = { { a = 'sector_move', sector = 1, surface = 'floor',
                  to = 'absolute', height = 4, speed = '0.5' } } },
} }
)LUA"));

    f.fire("on_cross", 0, {"ENT(0)", "11", "SEC(0)"});
    CHECK(f.truth("#MOVES == 1"));
    CHECK(f.truth("MOVES[1].x == fx.int(0)"));
    CHECK(f.truth("MOVES[1].y == fx.int(0)"));
    CHECK(f.truth("MOVES[1].z == fx.int(4)"));
    CHECK(f.truth("MOVES[1].ticks == 8"));
    // And no turn, which is the other half of the control: a runtime that put
    // a spin in every move would turn every door in every converted level.
    CHECK(f.truth("MOVES[1].spin == nil"));
}

static void test_a_moving_sector_refuses_a_second_start() {
    std::printf("triggers: a door already in flight is not restarted half-way\n");

    Fixture f;
    CHECK(f.open());
    CHECK(f.exec(R"LUA(
triggers.load{ name = 'door', triggers = {
  { name = 'open', event = 'on_interact', once = true,
    conditions = { { c = 'sector_at_rest', sector = 1 } },
    actions = { { a = 'sector_move', sector = 1, surface = 'ceiling',
                  to = 'absolute', height = 2, speed = '0.25' } } },
} }
)LUA"));

    f.exec("TICK = 0");
    f.fire("on_interact", 0, {"ENT(0)", "2", "SEC(0)"});
    CHECK(f.truth("#MOVES == 1"));
    CHECK(f.truth("triggers.is_spent('open') == true"));

    // A second press while it travels does nothing.
    f.exec("TICK = 3");
    f.fire("on_interact", 3, {"ENT(0)", "2", "SEC(0)"});
    CHECK(f.truth("#MOVES == 1"));
}

static void test_once_is_spent_only_on_success() {
    std::printf("triggers: a one-shot that did nothing is still armed\n");

    // Doom's own rule, and it matters: walking over a lift trigger while the
    // lift is already moving must not burn the trigger, or a player who arrived
    // early is locked out of the room for good.
    Fixture f;
    CHECK(f.open());
    CHECK(f.exec(R"LUA(
SECTORS[1].refuse = true
triggers.load{ name = 'once', triggers = {
  { name = 'the lift', event = 'on_cross', once = true,
    actions = { { a = 'sector_move', sector = 1, surface = 'floor',
                  to = 'lowest_neighbour_floor', speed = '0.125' } } },
} }
)LUA"));

    f.fire("on_cross", 1, {"ENT(0)", "11", "SEC(2)"});
    CHECK(f.truth("#MOVES == 0"));
    CHECK_MSG(f.truth("triggers.is_spent('the lift') == false"),
              "a one-shot whose action refused must not be spent");

    // CONTROL: let the same action succeed and it is spent, so "not spent" is
    // not simply what this runtime always reports.
    CHECK(f.exec("SECTORS[1].refuse = false"));
    f.fire("on_cross", 2, {"ENT(0)", "11", "SEC(2)"});
    CHECK(f.truth("#MOVES == 1"));
    CHECK(f.truth("triggers.is_spent('the lift') == true"));

    f.fire("on_cross", 3, {"ENT(0)", "11", "SEC(2)"});
    CHECK(f.truth("#MOVES == 1"));
}

static void test_a_sequence_is_beats_on_one_queue() {
    std::printf("triggers: a sequence fires its beats on their ticks, and once each\n");

    Fixture f;
    CHECK(f.open());
    CHECK(f.exec(R"LUA(
triggers.load{ name = 'cut', triggers = {
  { name = 'intro', event = 'on_interact', once = true,
    sequence = {
      { when = 0,  a = 'counter_add', counter = 'beat0' },
      { when = 3,  a = 'counter_add', counter = 'beat3' },
      { when = 10, a = 'counter_add', counter = 'beat10' },
    } },
} }
)LUA"));

    f.exec("TICK = 200");
    f.fire("on_interact", 200, {"ENT(0)", "2", "SEC(0)"});

    // at = 0 runs WITH the trigger, not one tick after it. A cutscene whose
    // first frame lands a tick late is a visible hitch at the moment the player
    // pressed the thing.
    CHECK(f.truth("triggers.counter('beat0') == 1"));
    CHECK(f.truth("triggers.counter('beat3') == 0"));
    CHECK(f.truth("triggers.pending() == 2"));

    for (u32 t = 201; t <= 202; ++t) f.tick(t);
    CHECK_MSG(f.truth("triggers.counter('beat3') == 0"), "a beat must not fire early");

    f.tick(203);
    CHECK(f.truth("triggers.counter('beat3') == 1"));
    CHECK(f.truth("triggers.counter('beat10') == 0"));
    CHECK(f.truth("triggers.pending() == 1"));

    for (u32 t = 204; t <= 209; ++t) f.tick(t);
    CHECK(f.truth("triggers.counter('beat10') == 0"));
    f.tick(210);
    CHECK(f.truth("triggers.counter('beat10') == 1"));
    CHECK(f.truth("triggers.pending() == 0"));

    // Once each, not every tick after their deadline.
    for (u32 t = 211; t <= 220; ++t) f.tick(t);
    CHECK(f.truth("triggers.counter('beat3') == 1"));
    CHECK(f.truth("triggers.counter('beat10') == 1"));

    // `once` is SPENT by queuing, not by a beat happening to report true. A
    // cutscene whose beats all land later would otherwise restart on every
    // event until one of them did.
    f.fire("on_interact", 221, {"ENT(0)", "2", "SEC(0)"});
    CHECK_MSG(f.truth("triggers.counter('beat0') == 1"),
              "a once-trigger with a sequence must be spent by firing it");
    CHECK(f.truth("triggers.pending() == 0"));
}

static void test_a_sequence_does_not_recheck_its_conditions() {
    std::printf("triggers: a sequence's conditions are checked when it fires, not per beat\n");

    // The rule that makes a cutscene survivable. Conditions belong to the
    // trigger and are tested once; a beat that re-tested them would stop a
    // cutscene halfway because the player walked out of the sector that
    // started it -- which is exactly what a player does when the camera takes
    // over and their input stops mattering.
    Fixture f;
    CHECK(f.open());
    CHECK(f.exec(R"LUA(
triggers.load{ name = 'guarded', triggers = {
  { name = 'shot', event = 'on_interact',
    conditions = { { c = 'sector_is', sector = 0 } },
    sequence = { { when = 2, a = 'counter_add', counter = 'late' } } },
} }
)LUA"));

    f.exec("TICK = 300");
    f.fire("on_interact", 300, {"ENT(0)", "2", "SEC(0)"});
    CHECK(f.truth("triggers.pending() == 1"));

    // A different event entirely arrives before the beat is due -- one whose
    // sector would NOT have satisfied the condition.
    f.fire("on_interact", 301, {"ENT(0)", "2", "SEC(1)"});
    f.tick(302);
    CHECK_MSG(f.truth("triggers.counter('late') == 1"),
              "the beat must fire on its deadline regardless of what happened since");
}

static void test_run_after_is_a_deadline_not_a_wait() {
    std::printf("triggers: run_after is a tick deadline; nothing suspends\n");

    Fixture f;
    CHECK(f.open());
    CHECK(f.exec(R"LUA(
triggers.load{ name = 'timer', triggers = {
  { name = 'open', event = 'on_interact',
    actions = { { a = 'counter_add', counter = 'opened' },
                { a = 'run_after', trigger = 'close', ticks = 5 } } },
  { name = 'close', actions = { { a = 'counter_add', counter = 'closed' } } },
} }
)LUA"));

    f.exec("TICK = 100");
    f.fire("on_interact", 100, {"ENT(0)", "2", "SEC(0)"});
    // The FIRST action ran to completion before the deadline was even queued:
    // nothing here suspends, so the rest of the trigger is not waiting on it.
    CHECK(f.truth("triggers.counter('opened') == 1"));
    CHECK(f.truth("triggers.counter('closed') == 0"));
    CHECK(f.truth("triggers.pending() == 1"));

    for (u32 t = 101; t <= 104; ++t) f.tick(t);
    CHECK_MSG(f.truth("triggers.counter('closed') == 0"),
              "a deadline must not fire early");
    CHECK(f.truth("triggers.pending() == 1"));

    f.tick(105);
    CHECK(f.truth("triggers.counter('closed') == 1"));
    CHECK(f.truth("triggers.pending() == 0"));

    // And it fires once, not every tick after.
    f.tick(106);
    f.tick(107);
    CHECK(f.truth("triggers.counter('closed') == 1"));
}

static void test_a_deadline_queued_by_a_deadline_survives() {
    std::printf("triggers: a deadline queued from inside a deadline is not dropped\n");

    // The bug this is written for: draining the live queue and assigning the
    // survivors back over it throws away every entry a callback added, because
    // `for i = 1, #timers` fixes the limit once. The symptom is a door that
    // opens and never shuts, with no error anywhere -- it reads as a feature
    // nobody implemented.
    Fixture f;
    CHECK(f.open());
    CHECK(f.exec(R"LUA(
triggers.load{ name = 'chain', triggers = {
  { name = 'start', event = 'on_interact',
    actions = { { a = 'run_after', trigger = 'middle', ticks = 1 } } },
  { name = 'middle', actions = { { a = 'counter_add', counter = 'middle' },
                                 { a = 'run_after', trigger = 'last', ticks = 1 } } },
  { name = 'last', actions = { { a = 'counter_add', counter = 'last' } } },
} }
)LUA"));

    f.exec("TICK = 10");
    f.fire("on_interact", 10, {"ENT(0)", "2", "SEC(0)"});
    f.tick(11);
    CHECK(f.truth("triggers.counter('middle') == 1"));
    f.tick(12);
    CHECK_MSG(f.truth("triggers.counter('last') == 1"),
              "the deadline queued while draining the queue was dropped");
}

static void test_run_is_bounded() {
    std::printf("triggers: two triggers that run each other stop, and say so\n");

    Fixture f;
    CHECK(f.open());
    CHECK(f.exec(R"LUA(
triggers.load{ name = 'cycle', triggers = {
  { name = 'ping', event = 'on_interact',
    actions = { { a = 'counter_add', counter = 'n' }, { a = 'run', trigger = 'pong' } } },
  { name = 'pong', actions = { { a = 'run', trigger = 'ping' } } },
} }
)LUA"));

    f.fire("on_interact", 1, {"ENT(0)", "2", "SEC(0)"});
    // Bounded, and bounded LOW: the instruction budget would eventually stop
    // this, but a budget kill is a dead callback with no name in it.
    CHECK(f.truth("triggers.counter('n') > 0"));
    CHECK(f.truth("triggers.counter('n') <= 8"));
}

static void test_enable_and_disable() {
    std::printf("triggers: a trigger can arm and disarm another\n");

    Fixture f;
    CHECK(f.open());
    CHECK(f.exec(R"LUA(
triggers.load{ name = 'gate', triggers = {
  { name = 'the switch', event = 'on_interact', enabled = false,
    actions = { { a = 'counter_add', counter = 'used' } } },
  { name = 'the key', event = 'on_touch',
    actions = { { a = 'enable', trigger = 'the switch' } } },
  { name = 'the alarm', event = 'on_crush',
    actions = { { a = 'disable', trigger = 'the switch' } } },
} }
)LUA"));

    f.fire("on_interact", 1, {"ENT(0)", "2", "SEC(0)"});
    CHECK(f.truth("triggers.counter('used') == 0"));
    CHECK(f.truth("triggers.is_enabled('the switch') == false"));

    f.fire("on_touch", 2, {"ENT(0)", "ENT(1)"});
    CHECK(f.truth("triggers.is_enabled('the switch') == true"));
    f.fire("on_interact", 3, {"ENT(0)", "2", "SEC(0)"});
    CHECK(f.truth("triggers.counter('used') == 1"));

    f.fire("on_crush", 4, {"ENT(0)", "SEC(1)", "'vertical'"});
    CHECK(f.truth("triggers.is_enabled('the switch') == false"));
    f.fire("on_interact", 5, {"ENT(0)", "2", "SEC(0)"});
    CHECK(f.truth("triggers.counter('used') == 1"));
}

static void test_music_reaches_the_player_or_everyone() {
    std::printf("triggers: a music cue goes to the event's player, or to everyone\n");

    Fixture f;
    CHECK(f.open());
    CHECK(f.exec(R"LUA(
triggers.load{ name = 'music', triggers = {
  { name = 'fight starts', event = 'on_attack',
    actions = { { a = 'music', cue = 'fight' },
                { a = 'music', cue = 'calm', fade = 200, at = 'now', everyone = true } } },
} }
)LUA"));

    f.fire("on_attack", 1, {"ENT(0)", "ENT(1)"});
    CHECK_MSG(f.truth("logged('music 0 fight nil nil')"), "the default is the event's player, with the binding's defaults");
    CHECK_MSG(f.truth("logged('music all calm 200 now')"), "everyone=true sends to nobody in particular");

    // CONTROL: the vocabulary refuses a moment it does not know. A fresh
    // fixture, because one map holds one table and a second load is refused
    // for that reason alone.
    Fixture g;
    CHECK(g.open());
    g.exec(R"LUA(
BAD = triggers.load{ name = 'bad', triggers = {
  { name = 'x', event = 'on_attack', actions = { { a = 'music', cue = 'fight', at = 'someday' } } },
} }
)LUA");
    CHECK_MSG(g.truth("BAD == false"), "at='someday' is not a moment the vocabulary knows");
}

static void test_who_selects_between_the_two_handles() {
    std::printf("triggers: an action says whether it means the actor or the target\n");

    Fixture f;
    CHECK(f.open());
    CHECK(f.exec(R"LUA(
triggers.load{ name = 'who', triggers = {
  { name = 'hit the imp', event = 'on_attack',
    conditions = { { c = 'target_name_is', name = 'imp' } },
    actions = { { a = 'set_animation', state = 1, who = 'target' },
                { a = 'set_hp', add = -10, who = 'target' },
                { a = 'set_hp', add = -1 } } },
} }
)LUA"));

    f.fire("on_attack", 1, {"ENT(0)", "ENT(1)"});
    CHECK(f.truth("logged('anim 1 1')"));
    CHECK_MSG(f.truth("logged('hp 1 90')"), "who='target' must reach the target");
    CHECK_MSG(f.truth("logged('hp 0 99')"), "the default must reach the actor");

    // CONTROL: the condition reads the TARGET's name, not the attacker's. The
    // attacker here is a player with no catalogue entry, so a condition that
    // was quietly reading the actor would never match and the trigger would
    // never fire -- which is not the same as it firing on the wrong handle.
    CHECK(f.exec("reset_log()"));
    f.fire("on_attack", 2, {"ENT(1)", "ENT(0)"});
    CHECK_MSG(f.truth("not logged('anim 0 1')"),
              "target_name_is must read the target, and a player is never a catalogue kind");
}

static void test_a_tag_addresses_every_sector_carrying_it() {
    std::printf("triggers: a tag names a set of sectors, not one\n");

    Fixture f;
    CHECK(f.open());
    CHECK(f.exec(R"LUA(
add_sector(4, 2, 0, 0, {})     -- a second sector tagged 2
triggers.load{ name = 'tags', triggers = {
  { name = 'raise them all', event = 'on_interact',
    actions = { { a = 'sector_move', tag = 2, surface = 'ceiling',
                  to = 'absolute', height = 3, speed = 1 } } },
} }
)LUA"));

    f.fire("on_interact", 1, {"ENT(0)", "2", "SEC(0)"});
    CHECK(f.truth("#MOVES == 2"));
    CHECK(f.truth("(MOVES[1].sector == 1 and MOVES[2].sector == 4) or "
                  "(MOVES[1].sector == 4 and MOVES[2].sector == 1)"));
}

static void test_an_exact_fraction_is_the_number_it_names() {
    std::printf("triggers: {n, d} is exact where a decimal cannot be\n");

    // 4/48 has no finite decimal. A Doom door opens four map units below the
    // ceiling above it, so without this form every trigger-authored door would
    // sit a rounding step away from every converted one -- and the two would
    // disagree about whether a body fits through.
    Fixture f;
    CHECK(f.open());
    CHECK(f.exec(R"LUA(
triggers.load{ name = 'frac', triggers = {
  { name = 'open', event = 'on_interact',
    actions = { { a = 'sector_move', sector = 1, surface = 'ceiling',
                  to = 'lowest_neighbour_ceiling', offset = {-4, 48},
                  speed = {1, 4} } } },
} }
)LUA"));

    f.fire("on_interact", 0, {"ENT(0)", "2", "SEC(0)"});
    CHECK(f.truth("#MOVES == 1"));
    CHECK_MSG(f.truth("MOVES[1].z == fx.int(2) - fx.frac(4, 48)"),
              "the fraction must arrive exactly");

    // CONTROL: the nearest decimal an author could actually write is NOT this
    // number. Without this the assertion above would also pass for a runtime
    // that rounded, since the two agree to six places.
    CHECK_MSG(f.truth("fx.frac(4, 48) ~= fx.parse('0.083333')"),
              "if a decimal could express 4/48 this whole form would be pointless");
}

static void test_a_wait_can_run_from_when_a_sector_settles() {
    std::printf("triggers: a door waits from when it finished opening, not from the button\n");

    Fixture f;
    CHECK(f.open());
    CHECK(f.exec(R"LUA(
triggers.load{ name = 'door', triggers = {
  { name = 'open', event = 'on_interact',
    actions = { { a = 'sector_move', sector = 1, surface = 'ceiling',
                  to = 'absolute', height = 2, speed = '0.25' },
                { a = 'run_after', trigger = 'shut', after = 1, ticks = 10 } } },
  { name = 'shut', actions = { { a = 'counter_add', counter = 'shut' } } },
} }
)LUA"));

    // The move is 2 units at 0.25 a tick: 8 ticks. The wait is 10 more, so the
    // close is due at 18 and not at 10.
    f.exec("TICK = 0");
    f.fire("on_interact", 0, {"ENT(0)", "2", "SEC(0)"});
    CHECK(f.truth("MOVES[1].ticks == 8"));

    for (u32 t = 1; t <= 17; ++t) f.tick(t);
    CHECK_MSG(f.truth("triggers.counter('shut') == 0"),
              "the wait started at the button rather than at the settle");
    f.tick(18);
    CHECK(f.truth("triggers.counter('shut') == 1"));
}

static void test_a_property_names_a_state() {
    std::printf("triggers: a property holds what a counter cannot\n");

    Fixture f;
    CHECK(f.open());
    CHECK(f.exec(R"LUA(
triggers.load{ name = 'prop', triggers = {
  { name = 'lock it', event = 'on_interact',
    conditions = { { c = 'side_is', side = 2, expect_neighbour = 1 } },
    actions = { { a = 'set_property', name = 'door', value = 'locked' } } },
  { name = 'try it', event = 'on_interact',
    conditions = { { c = 'side_is', side = 3, expect_neighbour = -1 },
                   { c = 'property_is', name = 'door', value = 'locked' } },
    actions = { { a = 'counter_add', counter = 'refused' } } },
  { name = 'open it', event = 'on_interact',
    conditions = { { c = 'side_is', side = 3, expect_neighbour = -1 },
                   { c = 'property_is', name = 'door', value = 'locked', negate = true } },
    actions = { { a = 'counter_add', counter = 'opened' } } },
} }
)LUA"));

    // Unset, so the door is not locked.
    f.fire("on_interact", 1, {"ENT(0)", "3", "SEC(0)"});
    CHECK(f.truth("triggers.counter('opened') == 1"));
    CHECK(f.truth("triggers.counter('refused') == 0"));

    f.fire("on_interact", 2, {"ENT(0)", "2", "SEC(0)"});
    CHECK(f.truth("triggers.property('door') == 'locked'"));

    f.fire("on_interact", 3, {"ENT(0)", "3", "SEC(0)"});
    CHECK(f.truth("triggers.counter('refused') == 1"));
    CHECK_MSG(f.truth("triggers.counter('opened') == 1"),
              "the negated condition must stop matching once the property is set");

    // ONE STORE, not two. A counter and a property of the same name are the
    // same thing -- two tables would let a map set one and read the other
    // forever without either being wrong about anything.
    CHECK(f.exec("triggers.set_property('n', 5)"));
    CHECK(f.truth("triggers.counter('n') == 5"));

    // And a counter comparison against a string answers false rather than
    // raising: `'locked' >= 1` is an error in Lua, and an error here would take
    // the rest of the trigger with it.
    Fixture g;
    CHECK(g.open());
    CHECK(g.exec(R"LUA(
triggers.load{ name = 'mixed', triggers = {
  { name = 'set it', event = 'on_touch',
    actions = { { a = 'set_property', name = 'k', value = 'a string' } } },
  { name = 'count it', event = 'on_interact',
    conditions = { { c = 'counter_at_least', counter = 'k', value = 1 } },
    actions = { { a = 'counter_add', counter = 'fired' } } },
} }
)LUA"));
    g.fire("on_touch", 1, {"ENT(0)", "ENT(1)"});
    g.fire("on_interact", 2, {"ENT(0)", "2", "SEC(0)"});
    CHECK_MSG(g.truth("triggers.counter('fired') == 0"),
              "comparing a counter against a string must be false, not an error");
}

static void test_custom_code_is_the_escape_hatch() {
    std::printf("triggers: custom code runs, sees the event, and is compiled at load\n");

    Fixture f;
    CHECK(f.open());
    CHECK(f.exec(R"LUA(
triggers.load{ name = 'code', triggers = {
  -- The `fn` form: a value in the table. Needs no compiler at run time, so it
  -- is the one that works under Trust::Strict.
  { name = 'by function', event = 'on_interact',
    conditions = { { c = 'side_is', side = 2, expect_neighbour = 1 } },
    actions = { { a = 'lua', fn = function (ev)
                    note(("fn saw side %d entity %d"):format(ev.side, ev.entity:index()))
                    triggers.set_property("via_fn", true)
                  end } } },

  -- The `code` form: source an editor can round-trip.
  { name = 'by source', event = 'on_interact',
    conditions = { { c = 'side_is', side = 3, expect_neighbour = -1 } },
    actions = { { a = 'lua', code =
                  'note("code saw side " .. ev.side); triggers.set_property("via_code", 7)' } } },

  -- Returning false means it did nothing, so a one-shot stays armed.
  { name = 'declines', event = 'on_cross', once = true,
    actions = { { a = 'lua', fn = function () return false end } } },
} }
)LUA"));

    f.fire("on_interact", 1, {"ENT(0)", "2", "SEC(0)"});
    CHECK(f.truth("logged('fn saw side 2 entity 0')"));
    CHECK(f.truth("triggers.property('via_fn') == true"));

    f.fire("on_interact", 2, {"ENT(0)", "3", "SEC(0)"});
    CHECK(f.truth("logged('code saw side 3')"));
    CHECK_MSG(f.truth("triggers.property('via_code') == 7"),
              "the `code` form must compile and run with `ev` in scope");

    f.fire("on_cross", 3, {"ENT(0)", "11", "SEC(2)"});
    CHECK_MSG(f.truth("triggers.is_spent('declines') == false"),
              "custom code returning false must not spend a one-shot");

    // CONTROL: the conditions in front of it still apply. Custom code is an
    // action, not a way around the rest of the trigger -- without this, "the
    // code ran" is satisfied by code that runs on every event.
    CHECK(f.exec("reset_log()"));
    f.fire("on_interact", 4, {"ENT(0)", "9", "SEC(0)"});
    CHECK_MSG(f.truth("not logged('code saw side 9')"),
              "custom code must still be gated by its trigger's conditions");
}

static void test_strict_refuses_source_and_still_takes_a_function() {
    std::printf("triggers: under Trust::Strict, `code` refuses and `fn` still works\n");

    // The reason the `lua` action has two forms. Trust::Strict removes
    // `loadstring`, so source cannot be compiled at all -- and an operator
    // running third-party maps in that profile must not get a map that starts
    // with one action quietly missing.
    Fixture f;
    CHECK(f.open(Trust::Strict));
    CHECK(f.truth("loadstring == nil"));

    CHECK(f.exec("A = triggers.load{ name='s', triggers = {"
                 "  { name='a', event='on_tick',"
                 "    actions={ {a='lua', code='return true'} } } } }"));
    CHECK_MSG(f.truth("A == false"),
              "a map using `code` must refuse to load where loadstring is gone");

    // CONTROL, and the reason `fn` exists: the SAME trust profile takes a
    // function, so the refusal above is about the compiler and not about the
    // action being unavailable under Strict.
    Fixture g;
    CHECK(g.open(Trust::Strict));
    CHECK(g.exec(R"LUA(
B = triggers.load{ name = 's', triggers = {
  { name = 'a', event = 'on_interact',
    actions = { { a = 'lua', fn = function (ev) note("strict fn ran") end } } },
} }
)LUA"));
    CHECK(g.truth("B == true"));
    g.fire("on_interact", 1, {"ENT(0)", "2", "SEC(0)"});
    CHECK(g.truth("logged('strict fn ran')"));
}

static void test_a_second_table_is_refused() {
    std::printf("triggers: a second trigger table does not silently replace the first\n");

    // The bug the VM's hook LISTS exist to prevent, one layer up: two scripts
    // that each wrote `function on_tick` used to lose the first silently, and a
    // map that loaded a door library and a monster library lost every door the
    // moment it gained a monster. Two trigger tables in one map is the same
    // shape, so it is refused rather than resolved.
    Fixture f;
    CHECK(f.open());
    CHECK(f.exec("A = triggers.load{ name='first', triggers = {"
                 "  { name='the door', event='on_interact',"
                 "    actions={ {a='counter_add', counter='first'} } } } }"));
    CHECK(f.truth("A == true"));

    CHECK(f.exec("B = triggers.load{ name='second', triggers = {"
                 "  { name='the lift', event='on_interact',"
                 "    actions={ {a='counter_add', counter='second'} } } } }"));
    CHECK(f.truth("B == false"));

    // And the first is untouched -- refusing must not take the loaded table
    // down with it.
    f.fire("on_interact", 1, {"ENT(0)", "2", "SEC(0)"});
    CHECK_MSG(f.truth("triggers.counter('first') == 1"),
              "the first table must still be live after the second was refused");
    CHECK(f.truth("triggers.counter('second') == 0"));
}

static void test_panels_are_checked_at_load() {
    std::printf("triggers: a bad panel is a load-time problem, not a dead callback\n");

    // dnp.ui_open RAISES on a bad widget, and a raise inside a callback takes
    // the rest of the trigger with it -- so without this, the first press both
    // fails to open the panel AND silently skips whatever came after it.
    const char* const broken[] = {
        // a widget kind that does not exist
        "panels = { p = { title='T', widgets = { {kind='dial', id=1, label='x'} } } },",
        // a slider with no range
        "panels = { p = { title='T', widgets = { {kind='slider', id=1, label='x'} } } },",
        // a select with no options
        "panels = { p = { title='T', widgets = { {kind='select', id=1, label='x'} } } },",
        // options that are not strings
        ("panels = { p = { title='T', widgets = "
         "  { {kind='select', id=1, label='x', options={1,2}} } } },"),
        // a title that is not text
        "panels = { p = { title=3, widgets={} } },",
    };
    for (const char* pn : broken) {
        Fixture f;
        if (!f.open()) { CHECK(false); continue; }
        std::string src = "BAD = triggers.load{ name='p', ";
        src += pn;
        src += " triggers = { { name='a', event='on_tick',"
               "  actions={ {a='ui_open', panel='p'} } } } }";
        f.exec(src.c_str());
        CHECK_MSG(f.truth("BAD == false"), pn);
    }

    // CONTROL: a correct panel of every kind loads. Without it the assertions
    // above are satisfied by a runtime that refuses all panels.
    Fixture g;
    CHECK(g.open());
    CHECK(g.exec(R"LUA(
OK = triggers.load{ name = 'p',
  panels = { shop = { title = 'Shop', widgets = {
    { kind = 'label',  id = 1, label = 'Wares' },
    { kind = 'button', id = 2, label = 'Buy' },
    { kind = 'toggle', id = 3, label = 'Lights', value = 1 },
    { kind = 'slider', id = 4, label = 'How many', lo = 1, hi = 9, value = 3 },
    { kind = 'select', id = 5, label = 'Colour', options = { 'red', 'blue' } },
  } } },
  triggers = {
    { name = 'open it', event = 'on_prop_use',
      actions = { { a = 'ui_open', panel = 'shop' } } },
  } }
)LUA"));
    CHECK(g.truth("OK == true"));
    g.fire("on_prop_use", 1, {"ENT(0)", "ENT(1)"});
    CHECK(g.truth("logged('ui_open Shop')"));

    // A panel name that resolves to nothing is the same class of mistake as a
    // `run` naming a trigger that is not there -- at fire time it is a silent
    // false forever.
    Fixture h;
    CHECK(h.open());
    CHECK(h.exec("MISSING = triggers.load{ name='p', triggers = {"
                 "  { name='a', event='on_tick', actions={ {a='ui_open', panel='nope'} } } } }"));
    CHECK(h.truth("MISSING == false"));
}

static void test_renumbered_geometry_is_caught_at_load() {
    std::printf("triggers: a map whose sides were renumbered refuses to load\n");

    // MEASURED, not imagined. Adding one vertex to sector 0 of
    // maps/doors_trig.map renumbers every side after it, and before this guard
    // existed the result was that the door, both trip-wires and the exit all
    // pointed at blank wall while the runtime reported "11 triggers on 4
    // events". A map where nothing works and nothing says why.
    //
    // Side indices come from the order mapc walks the .map, so this is not an
    // exotic edit -- it is what dragging a corner in the editor does.
    Fixture f;
    CHECK(f.open());
    CHECK(f.exec("A = triggers.load{ name='g', triggers = {"
                 "  { name='the door', event='on_interact',"
                 "    conditions={ {c='side_is', side=2, expect_neighbour=1} },"
                 "    actions={ {a='counter_add', counter='n'} } } } }"));
    CHECK(f.truth("A == true"));

    // The same table against a map that has been edited underneath it: side 2
    // is now a plain wall.
    Fixture g;
    CHECK(g.open());
    CHECK(g.exec("add_side(2)"));           // no neighbour any more
    CHECK(g.exec("B = triggers.load{ name='g', triggers = {"
                 "  { name='the door', event='on_interact',"
                 "    conditions={ {c='side_is', side=2, expect_neighbour=1} },"
                 "    actions={ {a='counter_add', counter='n'} } } } }"));
    CHECK_MSG(g.truth("B == false"),
              "a stale side index must refuse the table, not watch the wrong wall");
    CHECK_MSG(g.truth("triggers.is_enabled('the door') == false"),
              "and nothing of it may be left registered");

    // CONTROL: the guard is comparing, not simply refusing. The SAME edited map
    // loads a table whose guard was brought up to date -- so "B == false" above
    // is about the mismatch and not about this fixture rejecting everything.
    Fixture h;
    CHECK(h.open());
    CHECK(h.exec("add_side(2)"));
    CHECK(h.exec("C = triggers.load{ name='g', triggers = {"
                 "  { name='the door', event='on_interact',"
                 "    conditions={ {c='side_is', side=2, expect_neighbour=-1} },"
                 "    actions={ {a='counter_add', counter='n'} } } } }"));
    CHECK(h.truth("C == true"));

    // A side that is not in the map at all is the same class of mistake and is
    // named as such rather than as a mismatch.
    Fixture i;
    CHECK(i.open());
    CHECK(i.exec("D = triggers.load{ name='g', triggers = {"
                 "  { name='x', event='on_interact',"
                 "    conditions={ {c='side_is', side=900, expect_neighbour=1} },"
                 "    actions={ {a='counter_add', counter='n'} } } } }"));
    CHECK(i.truth("D == false"));

    // And the sector half of it. Sector indices come from declaration order, so
    // inserting a sector renumbers the ones after it; `expect_tag` is optional
    // because addressing a sector by `tag` needs no guard at all.
    Fixture j;
    CHECK(j.open());
    CHECK(j.exec("E = triggers.load{ name='g', triggers = {"
                 "  { name='x', event='on_tick',"
                 "    actions={ {a='sector_move', sector=1, expect_tag=2,"
                 "               surface='ceiling', to='absolute', height=2, speed=1} } } } }"));
    CHECK_MSG(j.truth("E == true"), "sector 1 is tagged 2 in this fixture");

    Fixture k;
    CHECK(k.open());
    CHECK(k.exec("F = triggers.load{ name='g', triggers = {"
                 "  { name='x', event='on_tick',"
                 "    actions={ {a='sector_move', sector=1, expect_tag=99,"
                 "               surface='ceiling', to='absolute', height=2, speed=1} } } } }"));
    CHECK_MSG(k.truth("F == false"), "a stale sector index must refuse the table");
}

// Read a whole file, or an empty string.
static std::string slurp(const std::string& path) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) return std::string();
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
    std::fclose(fp);
    return out;
}

static void test_the_json_vocabulary_is_current() {
    std::printf("triggers: the checked-in JSON vocabulary matches the Lua one\n");

    // The editor is JavaScript and cannot read a Lua table, so the vocabulary
    // is translated once by tools/vocabjson and the result is checked in. That
    // makes it a generated file living beside its source -- which in this
    // repository is a thing that has already been deleted once as an
    // unreferenced duplicate, and is otherwise a thing that goes quietly stale.
    //
    // So it is compared here, byte for byte, against a fresh emit through the
    // same emit.lua the generator uses. One serialiser, two callers.
    Fixture f;
    CHECK(f.open());
    const std::string emit = g_root + "/tools/vocabjson/emit.lua";
    if (!f.vm.run_file(emit.c_str(), "emit.lua")) {
        std::printf("  FAIL running %s: %s\n", emit.c_str(), f.vm.last_error());
        ++g_checks; ++g_fails;
        return;
    }

    lua_State* L = f.vm.state();
    lua_getglobal(L, "VOCAB_JSON");
    const char* fresh_c = lua_tostring(L, -1);
    CHECK(fresh_c != nullptr);
    const std::string fresh = fresh_c ? fresh_c : "";
    lua_pop(L, 1);

    const std::string on_disk = slurp(g_root + "/maps/scripts/trigger_vocab.json");
    CHECK_MSG(!on_disk.empty(), "maps/scripts/trigger_vocab.json is missing");
    if (on_disk != fresh) {
        std::printf("  FAIL maps/scripts/trigger_vocab.json is out of date -- regenerate it:\n"
                    "         ./scripts/build_lua_tests.sh && \\\n"
                    "         ./build/lua/trigger_vocab_json . > maps/scripts/trigger_vocab.json\n");
        ++g_checks; ++g_fails;
    } else {
        ++g_checks;
    }

    // CONTROL: the emitter reflects the TABLE rather than returning a constant.
    // Change the vocabulary and the JSON must change with it -- without this, a
    // generator that emitted the checked-in file verbatim would pass above.
    CHECK(f.exec("trigger_vocab.actions[1].name = 'sector_shuffle'"));
    if (!f.vm.run_file(emit.c_str(), "emit.lua")) { CHECK(false); return; }
    lua_getglobal(L, "VOCAB_JSON");
    const char* changed = lua_tostring(L, -1);
    CHECK_MSG(changed != nullptr && fresh != changed,
              "the emitter ignored a change to the vocabulary");
    CHECK_MSG(changed != nullptr &&
                  std::string(changed).find("sector_shuffle") != std::string::npos,
              "and the change did not reach its output");
    lua_pop(L, 1);
}

static void test_the_document_and_the_vocabulary_agree() {
    std::printf("triggers: docs/TRIGGERS.md documents exactly the vocabulary\n");

    // The one-implementation rule, checked across a language boundary. The
    // document is what an author reads and the table is what the runtime and
    // the editor obey; a name in one and not the other is a feature that either
    // does not exist or cannot be found.
    const std::string path = g_root + "/../docs/TRIGGERS.md";
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) {
        std::printf("  FAIL cannot open %s\n", path.c_str());
        ++g_checks; ++g_fails;
        return;
    }
    std::string doc;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) doc.append(buf, n);
    std::fclose(fp);

    Fixture f;
    CHECK(f.open());
    // Every name, from the table, as one newline-separated string. Enumerated
    // in Lua rather than parsed out of the file in C++, because a second reader
    // of the same table is the very thing this test exists to prevent.
    CHECK(f.exec(R"LUA(
local out = {}
for _, e in ipairs(trigger_vocab.events)     do out[#out+1] = e.name end
for _, c in ipairs(trigger_vocab.conditions) do out[#out+1] = c.name end
for _, a in ipairs(trigger_vocab.actions)    do out[#out+1] = a.name end
NAMES = table.concat(out, "\n")
)LUA"));

    lua_State* L = f.vm.state();
    lua_getglobal(L, "NAMES");
    const char* names = lua_tostring(L, -1);
    CHECK(names != nullptr);
    std::vector<std::string> list;
    if (names != nullptr) {
        std::string cur;
        for (const char* p = names; ; ++p) {
            if (*p == '\n' || *p == '\0') {
                if (!cur.empty()) list.push_back(cur);
                cur.clear();
                if (*p == '\0') break;
            } else {
                cur.push_back(*p);
            }
        }
    }
    lua_pop(L, 1);

    CHECK_MSG(list.size() > 30, "the vocabulary should not be nearly empty");

    int missing = 0;
    for (const std::string& name : list) {
        const std::string heading = "### `" + name + "`";
        if (doc.find(heading) == std::string::npos) {
            ++missing;
            std::printf("  FAIL docs/TRIGGERS.md has no `%s` section\n", name.c_str());
        }
    }
    CHECK_MSG(missing == 0, "every vocabulary name must be documented");

    // CONTROL: the detector can fail. A name that is not in the vocabulary must
    // not be found in the document either -- without this, a search that
    // matched everything would pass the loop above silently.
    CHECK_MSG(doc.find("### `on_footstep`") == std::string::npos,
              "the heading search matches things that are not there");

    // And the other direction: a heading in the document that names nothing.
    int stray = 0;
    size_t at = 0;
    while ((at = doc.find("### `", at)) != std::string::npos) {
        const size_t start = at + 5;
        const size_t end = doc.find('`', start);
        if (end == std::string::npos) break;
        const std::string name = doc.substr(start, end - start);
        bool known = false;
        for (const std::string& k : list) if (k == name) known = true;
        if (!known) {
            ++stray;
            std::printf("  FAIL docs/TRIGGERS.md documents `%s`, which is not in the "
                        "vocabulary\n", name.c_str());
        }
        at = end;
    }
    CHECK_MSG(stray == 0, "the document must not describe things that do not exist");
}

int main(int argc, char** argv) {
    if (argc > 1) g_root = argv[1];
    else if (const char* e = std::getenv("DNP_ROOT")) g_root = e;

    std::printf("== server/lua trigger runtime suite ==\n");
    test_the_vocabulary_and_the_runtime_agree();
    test_a_broken_table_loads_nothing();
    test_entity_in_sector_tag();
    test_conditions_select();
    test_a_door_is_a_sector_and_a_side();
    test_a_sector_can_be_moved_sideways();
    test_a_sector_can_be_spun();
    test_a_move_with_no_travel_is_still_vertical();
    test_a_moving_sector_refuses_a_second_start();
    test_once_is_spent_only_on_success();
    test_run_after_is_a_deadline_not_a_wait();
    test_a_sequence_is_beats_on_one_queue();
    test_a_sequence_does_not_recheck_its_conditions();
    test_a_deadline_queued_by_a_deadline_survives();
    test_run_is_bounded();
    test_enable_and_disable();
    test_who_selects_between_the_two_handles();
    test_music_reaches_the_player_or_everyone();
    test_a_tag_addresses_every_sector_carrying_it();
    test_an_exact_fraction_is_the_number_it_names();
    test_a_wait_can_run_from_when_a_sector_settles();
    test_a_property_names_a_state();
    test_custom_code_is_the_escape_hatch();
    test_strict_refuses_source_and_still_takes_a_function();
    test_a_second_table_is_refused();
    test_panels_are_checked_at_load();
    test_renumbered_geometry_is_caught_at_load();
    test_the_json_vocabulary_is_current();
    test_the_document_and_the_vocabulary_agree();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
