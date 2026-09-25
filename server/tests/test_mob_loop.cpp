//
// The decision loop: does a mob do what its graph says?
//
// The graph test next door proves the ARRAYS say what the tables said. This
// proves the loop SPENDS them -- that a state's sequence fires at the ticks it
// declares, that a transition fires when its conditions hold and not before,
// that `goto` stops the rest of a sequence, and that an action the loop does
// not run reaches whoever is meant to run it, in order, once.
//
// AGAINST A REAL WORLD, not a double. The mob is an agent in a sim::World on a
// real room, because half the conditions ask the world something -- where it
// is, what it is standing on, whether a ray reaches you -- and a double would
// be a second answer to all of it. The graph is small and written here, which
// is the one thing that IS a fixture: doom_monsters is covered by the graph
// test, and what this needs is a state machine whose every edge is exercised
// inside a hundred ticks.
//
// THE SINK IS A RECORDER. Actions that stay in Lua are handed to an ActionSink,
// so here they are handed to one that writes down the tick, the state and the
// index. That is the assertion this file is built around: "the loop called
// exactly these, in this order, at these ticks" is a much stronger statement
// than "the monster ended up somewhere plausible".
//
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "lua/vm.hpp"
#include "mobs/mob_runtime.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

using namespace dnp;
using namespace dnp::server::lua;
using namespace dnp::server::mobs;

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
static fx F(i64 n) { return fx::from_int(n); }

// A room big enough to stand two bodies apart in.
struct Room {
    geom::MapStorage store;
    geom::MapData    view{};
    Room() {
        const fx2 p[] = {{F(0), F(0)}, {F(60), F(0)}, {F(60), F(60)}, {F(0), F(60)}};
        store.add_sector(p, 4, F(0), F(40), 1);
        for (u16 i = 0; i < 4; ++i) {
            store.add_side(0, i, static_cast<u16>((i + 1) % 4), geom::NO_SECTOR, 0);
        }
        store.rebuild_grid();
    }
    void attach(sim::World& w) {
        view = store.view();
        w.set_map(&view);
        w.set_map_storage(&store);
    }
};

// Every action the loop handed back, with when and what.
struct Recorder : ActionSink {
    struct Call { u32 tick; std::string state; u16 index; };
    std::vector<Call> calls;
    u32 now = 0;
    // When set, the recorder does what a Lua action that goes somewhere
    // actually does: it calls mob_enter. Not a flag the sink returns -- the
    // runtime works that out by watching, and faking the answer here would
    // test the fake.
    MobRuntime* rt = nullptr;
    u32 entity = 0;
    const char* leaves_to = nullptr;
    void run(u32 /*entity*/, const char* state_name, u16 seq_index) override {
        calls.push_back(Call{now, state_name, seq_index});
        if (leaves_to && rt) mob_enter(*rt, entity, leaves_to, std::strlen(leaves_to), now);
    }
};

// The fixture graph, written in the vocabulary and validated by the real
// loader -- so a typo here is a loader complaint rather than a test that
// quietly checks a graph nobody could author.
static const char* const FIXTURE = R"LUA(
dnp = dnp or {}
dnp.tick = function() return 0 end
dnp.players = function() return {} end
dnp.entities = function() return {} end
dnp.random = function(a) return a end
)LUA";

static const char* const GRAPH = R"LUA(
mobs.load{
  mobs = {
    dummy = {
      hp = 100,
      prop = 'dummy',
      initial = 'idle',
      vars = { ready = 0, shots = 0 },
      states = {
        idle = {
          anim = 0, move = 'stop',
          transitions = {
            { to = 'chase', when = { { cond = 'has_target' } } },
          },
        },
        chase = {
          anim = 1, move = 'chase', face = 'target',
          transitions = {
            { to = 'idle',   when = { { cond = 'has_target', negate = true } } },
            { to = 'attack', when = { { cond = 'target_within', distance = '8' },
                                      { cond = 'cooldown_ready', var = 'ready' } } },
            -- Reachable by the loader's walk and never taken at run time: the
            -- test enters `leave` directly, which is exactly what Lua's
            -- `hurt` and `revive` do. Without this edge the loader is right to
            -- call it an orphan, and a fixture the loader complains about is
            -- not a graph anybody could author.
            { to = 'leave',  when = { { cond = 'var_at_least', var = 'shots', value = 9999 } } },
          },
        },
        attack = {
          anim = 2, move = 'stop', ticks = 40, next = 'chase',
          sequence = {
            { at = 0,  action = 'set_anim', anim = 3 },
            { at = 4,  action = 'hitscan', damage = 5, range = '20' },
            { at = 6,  action = 'add_var', var = 'shots', by = 1 },
            { at = 8,  action = 'set_cooldown', var = 'ready', ticks = 60 },
          },
        },
        leave = {
          anim = 9, move = 'stop', ticks = 8, next = 'idle',
          sequence = {
            { at = 0, action = 'goto', state = 'idle' },
            { at = 0, action = 'hitscan', damage = 1, range = '1' },
          },
        },
      },
    },
  },
}
)LUA";

static bool run(Vm& vm, const char* chunk, const char* name) {
    lua_State* L = vm.state();
    vm.begin_tick();
    if (luaL_loadbuffer(L, chunk, std::strlen(chunk), name) != 0 ||
        lua_pcall(L, 0, 0, 0) != 0) {
        std::printf("  FAIL %s: %s\n", name, lua_tostring(L, -1));
        lua_pop(L, 1);
        ++g_fails; ++g_checks;
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc > 1) g_root = argv[1];
    std::printf("== the mob decision loop ==\n");

    Vm vm;
    VmConfig cfg;
    cfg.budget_per_call = 200000000;
    cfg.budget_per_tick = 200000000;
    if (!vm.open(cfg)) { std::printf("  FAIL vm.open: %s\n", vm.last_error()); return 1; }
    if (!run(vm, FIXTURE, "fixture")) return 1;
    {
        lua_State* L = vm.state();
        lua_getglobal(L, "dnp");
        open_mobs(L);
        lua_pop(L, 1);
    }
    vm.begin_tick();
    if (!vm.run_file((g_root + "/maps/scripts/mob_vocab.lua").c_str(), "vocab")) {
        std::printf("  FAIL vocab: %s\n", vm.last_error()); return 1;
    }
    vm.begin_tick();
    if (!vm.run_file((g_root + "/maps/scripts/mobs.lua").c_str(), "mobs")) {
        std::printf("  FAIL mobs.lua: %s\n", vm.last_error()); return 1;
    }
    if (!run(vm, GRAPH, "graph")) return 1;

    lua_State* L = vm.state();
    {
        lua_getglobal(L, "mobs");
        lua_getfield(L, -1, "problems");
        const int n = static_cast<int>(lua_objlen(L, -1));
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            std::printf("  loader: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 2);
        CHECK(n == 0);
    }

    static Room room;
    static sim::World world;
    world.reset();
    room.attach(world);

    MobRuntime rt;
    rt.reset();
    rt.graph = graph_of(L);
    rt.world = &world;
    CHECK(rt.graph.kinds.size() == 1);

    Recorder rec;

    // The mob, and nobody to hunt yet.
    const u32 mob_e = world.spawn(fx3{F(10), F(10), F(0)});
    world.agent_mut(mob_e).kind = 7;          // a catalogue kind: not a player
    world.agent_mut(mob_e).scripted = true;
    CHECK(track(rt, mob_e, "dummy", 5, false, 0));

    Mob* m = mob_of(rt, mob_e);
    CHECK(m != nullptr);
    CHECK(m->state == rt.graph.state_by_name(rt.graph.kinds[0], "idle", 4));
    CHECK(m->hp == 100 && m->max_hp == 100);

    // --- with no player, nothing happens ------------------------------------
    //
    // The CONTROL for everything below: the level sleeps with nobody in it, so
    // a mob that advanced here would make every later assertion meaningless --
    // "it chased" is otherwise satisfied by anything that moves on a timer.
    for (u32 t = 1; t <= 40; ++t) { rec.now = t; tick_mobs(rt, t, &rec); world.step(); }
    CHECK(m->state == rt.graph.state_by_name(rt.graph.kinds[0], "idle", 4));
    CHECK(rec.calls.empty());
    std::printf("  with no player: still '%s', %zu actions\n",
                rt.graph.str(rt.graph.states[m->state].name), rec.calls.size());

    // --- a player arrives ---------------------------------------------------
    const u32 player = world.spawn(fx3{F(14), F(10), F(0)});
    world.agent_mut(player).kind = 0;          // kind 0 IS the test for a player

    u32 tick = 41;
    const auto step = [&](u32 n) {
        for (u32 i = 0; i < n; ++i, ++tick) { rec.now = tick; tick_mobs(rt, tick, &rec); world.step(); }
    };

    step(THINK * 2);
    const i32 attack = rt.graph.state_by_name(rt.graph.kinds[0], "attack", 6);
    // Four units apart, well inside the eight the graph asks for, and the
    // cooldown starts ready -- so it goes idle -> chase -> attack within two
    // of its own think ticks.
    CHECK(m->state == attack);
    std::printf("  a player arrives: it reached '%s'\n",
                rt.graph.str(rt.graph.states[rt.graph.kinds[0].first_state + m->state].name));

    // --- the sequence fires at the ticks it declares ------------------------
    //
    // `at` is relative to entering the state and is a DEADLINE: the mob only
    // thinks every THINK-th tick, so an entry at 4 fires on the first think AT
    // OR AFTER 4 -- never before it, and never twice.
    const u32 entered = m->entered;
    rec.calls.clear();
    step(THINK * 8);
    CHECK(rec.calls.size() == 1);
    if (rec.calls.size() == 1) {
        const Recorder::Call& c = rec.calls[0];
        CHECK(c.state == "attack");
        CHECK(c.index == 2);                       // the hitscan, entry 2 of 4
        CHECK(c.tick >= entered + 4);              // never early
        std::printf("  the hitscan reached Lua once, at tick %u (entered %u, at=4)\n",
                    c.tick, entered);
    }

    // The three the loop runs ITSELF never reach the sink, and they had their
    // effect: `shots` counted up and `ready` became an absolute tick ahead.
    const i32* shots = mob_var(rt, *m, "shots", 5);
    const i32* ready = mob_var(rt, *m, "ready", 5);
    CHECK(shots != nullptr && *shots == 1);
    CHECK(ready != nullptr && *ready >= static_cast<i32>(entered + 8 + 60));
    std::printf("  and set_anim, add_var and set_cooldown did not: shots=%d ready=%d\n",
                shots ? *shots : -1, ready ? *ready : -1);

    // --- goto stops the rest of the sequence --------------------------------
    //
    // The `leave` state fires a goto at tick 0 and a hitscan at tick 0. The
    // hitscan must NEVER run: the rest of a sequence belongs to a state the
    // mob has left, and running it is the bug that makes a dying monster fire
    // one last shot.
    rec.calls.clear();
    const i32 leave = rt.graph.state_by_name(rt.graph.kinds[0], "leave", 5);
    CHECK(mob_enter(rt, mob_e, "leave", 5, tick));
    CHECK(m->state == leave);
    step(THINK);
    // It left, and the hitscan that shares the goto's tick never reached Lua.
    // Asserted as "not `leave`" rather than "is `idle`": the goto put it in
    // idle, and by the next think it has seen the player again and moved on.
    // Pinning the later state would be asserting the chase rule twice and
    // would break the day the fixture's distances change.
    CHECK(m->state != leave);
    CHECK(rec.calls.empty());
    std::printf("  goto stopped the sequence: %zu actions ran after it\n", rec.calls.size());

    // --- and a Lua action that leaves stops it too ---------------------------
    //
    // The sink's answer is the mob having gone somewhere -- a death, or an
    // action that called goto. Same rule, other direction.
    rec.calls.clear();
    rec.rt = &rt;
    rec.entity = mob_e;
    rec.leaves_to = "idle";
    CHECK(mob_enter(rt, mob_e, "attack", 6, tick));
    // TWO THINKS, which is exactly one attack episode: the first runs the
    // entries due at age 0, the second the ones due at 4 and after. Stepping
    // further is not a stronger test, it is a second episode -- the mob goes
    // back to idle, sees the player again and attacks again, which is correct
    // and would make the count below mean nothing.
    step(THINK * 2);
    // Entry 2 is the first that reaches Lua, and it went somewhere; entries 3
    // and 4 belong to a state the mob is no longer in and must not run.
    CHECK(rec.calls.size() == 1);
    CHECK(m->state != attack);
    std::printf("  a Lua action that goes somewhere stops it too: %zu action(s), now '%s'\n",
                rec.calls.size(),
                rt.graph.str(rt.graph.states[rt.graph.kinds[0].first_state + m->state].name));
    rec.leaves_to = nullptr;

    // --- a kind the loop cannot run is refused, not half-tracked -------------
    {
        MobRuntime other;
        other.reset();
        other.graph = rt.graph;
        other.world = &world;
        other.graph.kinds[0].unsupported = "lua (action)";
        const u32 e2 = world.spawn(fx3{F(30), F(30), F(0)});
        world.agent_mut(e2).kind = 7;
        CHECK(!track(other, e2, "dummy", 5, false, tick));
        CHECK(mob_of(other, e2) == nullptr);
        std::printf("  an unsupported kind is refused outright, not half-tracked\n");
    }

    // --- the height pair, at heights chosen rather than hoped for -----------
    //
    // target_above and target_below are a comparison between two feet, and
    // maps/mobvocab.map cannot reliably produce one: the monsters converge on
    // a stationary player and end up on its floor, so the fixture reaches
    // `peer_down` once in fourteen seconds and `peer_up` never. An edge that
    // does not fire proves nothing about the condition on it -- which is the
    // trap this whole fixture was rebuilt around -- so these two are checked
    // here instead, where the positions are set rather than waited for.
    {
        Mob probe;
        probe.entity = mob_e;
        probe.kind = 0;
        probe.has_tp = true;
        probe.p = fx3{F(0), F(0), F(10)};
        EvalWorld ev{&world, 100, nullptr, &rt.graph, &rt.mobs, nullptr, nullptr, nullptr};

        const auto ask = [&](Cond id, fx by, fx target_z) {
            Condition c;
            c.id = id;
            c.f = by;
            probe.tp = fx3{F(0), F(0), target_z};
            return evaluate(rt.graph, rt.graph.kinds[0], probe, c, ev, nullptr);
        };

        // Three units above, asking for two: yes above, no below.
        CHECK(ask(Cond::TargetAbove, F(2), F(13)));
        CHECK(!ask(Cond::TargetBelow, F(2), F(13)));
        // Three below, the other way round.
        CHECK(!ask(Cond::TargetAbove, F(2), F(7)));
        CHECK(ask(Cond::TargetBelow, F(2), F(7)));
        // EXACTLY the threshold counts, which is what `by` reads as
        // everywhere else in the vocabulary: at least this much.
        CHECK(ask(Cond::TargetAbove, F(2), F(12)));
        CHECK(ask(Cond::TargetBelow, F(2), F(8)));
        // And one short of it does not -- the control for the comparison
        // being a comparison rather than a sign test.
        CHECK(!ask(Cond::TargetAbove, F(2), F(11)));
        CHECK(!ask(Cond::TargetBelow, F(2), F(9)));
        // Level ground with no threshold: neither, and both, is wrong. `by`
        // absent means zero, so level satisfies both by the >= rule.
        CHECK(ask(Cond::TargetAbove, fx::zero(), F(10)));
        CHECK(ask(Cond::TargetBelow, fx::zero(), F(10)));
        // No target is no answer, whatever the heights say.
        probe.has_tp = false;
        CHECK(!ask(Cond::TargetAbove, F(2), F(13)));
        CHECK(!ask(Cond::TargetBelow, F(2), F(7)));
        std::printf("  the height pair, at set heights: above/below/level/absent\n");
    }

    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
