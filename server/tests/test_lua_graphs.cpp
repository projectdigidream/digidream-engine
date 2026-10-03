//
// GRAPHS (docs/GRAPHS.md), against the real bindings and a real world: the
// JSON a graph arrives as, the runtime that walks it (maps/scripts/graphs.lua),
// and the trigger verbs it runs through triggers.verbs.
//
// Effects are watched through the trigger runtime's own counters
// (triggers.counter), which a graph's `counter_add` action writes: the verb
// really is the triggers' verb, so the number really is the triggers' number.
//
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

#include "dnp/sim/world.hpp"
#include "dnp/geom/mapstore.hpp"
#include "lua/vm.hpp"
#include "lua/world_binding.hpp"
#include "lua/json.hpp"
#include "lua/commands.hpp"
#include "lua/effects.hpp"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

using namespace dnp;

static int g_checks = 0;
static int g_fails = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_fails;                                                     \
            std::printf("  FAIL %s:%d   %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

static fx F(i64 n) { return fx::from_int(n); }

struct Room {
    geom::MapStorage store;
    geom::MapData    view{};
    Room() {
        const fx2 p[] = {{-F(20), -F(20)}, {F(20), -F(20)}, {F(20), F(20)}, {-F(20), F(20)}};
        store.add_sector(p, 4, fx::zero(), F(10), 1);
        for (u16 i = 0; i < 4; ++i) store.add_side(0, i, static_cast<u16>((i + 1) % 4), geom::NO_SECTOR, 0);
        view = store.view();
    }
};

static lua_State* L = nullptr;
static server::lua::Vm vm;

static bool run(const char* src, const char* what) {
    if (luaL_dostring(L, src) != 0) {
        std::printf("  FAIL %s: %s\n", what, lua_tostring(L, -1));
        ++g_fails;
        lua_pop(L, 1);
        return false;
    }
    return true;
}
static double num(const char* global) {
    lua_getglobal(L, global);
    const double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1;
    lua_pop(L, 1);
    return v;
}
static bool truthy(const char* global) {
    lua_getglobal(L, global);
    const bool v = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}

// Decode JSON into global `name`; false if the decoder refused it.
static bool json_to(const char* name, const char* text, std::string* err_out = nullptr) {
    std::string err;
    if (!server::lua::push_json(L, text, std::strlen(text), err)) {
        if (err_out) *err_out = err;
        return false;
    }
    lua_setglobal(L, name);
    return true;
}

// Load a graph given as JSON; returns graphs.load's verdict.
static bool load_graph(const char* json) {
    if (!json_to("g_json", json)) { std::printf("  (the JSON itself was refused)\n"); return false; }
    if (!run("g_ok = graphs.load(g_json, g_json.name)", "graphs.load")) return false;
    return truthy("g_ok");
}

// A server tick as the server runs one: the hooks, then the step, so dnp.tick()
// moves -- a delay is measured against it.
static sim::World* g_world = nullptr;
static void ticks(int n) {
    for (int i = 0; i < n; ++i) { vm.call_hook("on_tick", g_world->tick()); g_world->step(); }
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && chdir(argv[1]) != 0) { std::printf("  FAIL chdir\n"); return 1; }
    std::printf("== graphs ==\n");

    static Room room;
    static sim::World world;
    world.reset();
    world.set_map(&room.view);
    world.set_map_storage(&room.store);
    const u32 player = world.spawn(fx3{fx::zero(), fx::zero(), fx::zero()});
    (void)player;
    g_world = &world;

    static server::lua::CommandQueue commands;
    static server::lua::EffectStack effects;
    static server::lua::WorldContext ctx;
    static i32 hp[sim::MAX_AGENTS];
    ctx.world = &world;
    ctx.map_store = &room.store;
    ctx.commands = &commands;
    ctx.effects = &effects;
    ctx.hp = hp;
    ctx.rng = Rng(20261003u, 5);

    server::lua::VmConfig cfg;
    if (!vm.open(cfg)) { std::printf("  FAIL vm.open\n"); return 1; }
    server::lua::open_world(vm.state(), &ctx);
    L = vm.state();
    for (const char* s : {"maps/scripts/trigger_vocab.lua", "maps/scripts/triggers.lua",
                          "maps/scripts/graph_vocab.lua", "maps/scripts/graphs.lua"}) {
        if (!vm.run_file(s, s)) { std::printf("  FAIL loading %s: %s\n", s, vm.last_error()); return 1; }
    }

    // ------------------------------------------------------------ the JSON
    std::printf("json: a graph's file, decoded\n");
    CHECK(json_to("j", R"({"a": [1, 2.5, -3e2], "b": {"c": "x\"yé😀"}, "t": true, "f": false})"));
    CHECK(run("j_ok = #j.a == 3 and j.a[2] == 2.5 and j.a[3] == -300 and j.t == true and j.f == false "
              "and j.b.c == 'x\"y\\195\\169\\240\\159\\152\\128'", "reading it"));
    CHECK(truthy("j_ok"));
    // Refusals, each with a reason -- and CONTROL: the valid one above passed.
    std::string why;
    CHECK(!json_to("j", R"({"a": null})", &why) && why.find("null") != std::string::npos);
    CHECK(!json_to("j", R"({"a": 1} x)", &why));
    CHECK(!json_to("j", R"({"a": 1)", &why));
    CHECK(!json_to("j", R"(["\ud83d"])", &why));
    CHECK(!json_to("j", R"({"a": 01})", &why));
    {
        std::string deep(70, '[');
        deep += std::string(70, ']');
        CHECK(!json_to("j", deep.c_str(), &why) && why.find("deep") != std::string::npos);
    }

    // ------------------------------------------------------ the vocabulary
    std::printf("vocabulary: every node implemented, nothing implemented that is not listed\n");
    CHECK(run("vocab_wrong = #graphs.check_vocabulary()", "checking"));
    CHECK(num("vocab_wrong") == 0);
    // CONTROL: a node the vocabulary lists and the runtime does not run is found.
    CHECK(run("graph_vocab.nodes[#graph_vocab.nodes + 1] = { name = 'teleport_everyone', args = {} }\n"
              "vocab_found = #graphs.check_vocabulary()\n"
              "graph_vocab.nodes[#graph_vocab.nodes] = nil", "a vocabulary entry with no runtime"));
    CHECK(num("vocab_found") == 1);

    // ------------------------------------------------- counting, branching
    std::printf("run: a counter, a comparison, a branch, once\n");
    CHECK(load_graph(R"({
      "graph": 1, "name": "count",
      "vars": { "ticks": 0 },
      "nodes": [
        { "id": "t",    "type": "event",   "event": "on_tick" },
        { "id": "inc",  "type": "add_var", "var": "ticks", "value": 1 },
        { "id": "get",  "type": "get_var", "var": "ticks" },
        { "id": "ten",  "type": "compare", "op": ">=", "b": 10 },
        { "id": "if",   "type": "branch" },
        { "id": "one",  "type": "once" },
        { "id": "hit",  "type": "action",  "a": "counter_add", "counter": "reached_ten" },
        { "id": "mark", "type": "action",  "a": "counter_add", "counter": "after_ten" }
      ],
      "links": [
        ["t.then", "inc.in"], ["inc.then", "if.in"],
        ["get.value", "ten.a"], ["ten.value", "if.cond"],
        ["if.true", "one.in"], ["one.then", "hit.in"]
      ],
      "meta": { "layout": "the studio's" }
    })"));
    ticks(25);
    CHECK(run("c_ticks = graphs.var('count', 'ticks'); c_ten = triggers.counter('reached_ten')", "reading"));
    std::printf("  after 25 ticks: ticks=%d, reached ten %d time(s)\n", (int)num("c_ticks"), (int)num("c_ten"));
    CHECK(num("c_ticks") == 25);
    CHECK(num("c_ten") == 1);    // once, not on every tick past ten

    // ----------------------------------------------- sequence, delay, data
    std::printf("run: a sequence in order, a delay, one draw per run\n");
    CHECK(load_graph(R"({
      "graph": 1, "name": "flow",
      "vars": { "order": "", "start": 0, "a": 0, "b": 0, "go": true },
      "nodes": [
        { "id": "t",   "type": "event",    "event": "on_tick" },
        { "id": "gate","type": "once" },
        { "id": "seq", "type": "sequence", "count": 3 },
        { "id": "s0",  "type": "set_var",  "var": "order", "value": "0" },
        { "id": "s1",  "type": "set_var",  "var": "order", "value": "1" },
        { "id": "s2",  "type": "set_var",  "var": "order", "value": "2" },
        { "id": "r",   "type": "random",   "lo": 1, "hi": 1000000 },
        { "id": "ra",  "type": "set_var",  "var": "a" },
        { "id": "rb",  "type": "set_var",  "var": "b" },
        { "id": "wait","type": "delay",    "ticks": 30 },
        { "id": "late","type": "action",   "a": "counter_add", "counter": "late", "by": 2 }
      ],
      "links": [
        ["t.then", "gate.in"], ["gate.then", "seq.in"],
        ["seq.0", "s0.in"], ["seq.1", "s1.in"], ["seq.2", "s2.in"],
        ["s0.then", "ra.in"], ["ra.then", "rb.in"],
        ["r.value", "ra.value"], ["r.value", "rb.value"],
        ["s2.then", "wait.in"], ["wait.then", "late.in"]
      ]
    })"));
    ticks(1);
    CHECK(run("f_order = graphs.var('flow', 'order'); f_same = graphs.var('flow', 'a') == graphs.var('flow', 'b') "
              "and graphs.var('flow', 'a') ~= 0; f_late = triggers.counter('late'); f_pending = graphs.pending()", "reading"));
    CHECK(run("f_order_is_2 = f_order == '2'", "order"));
    CHECK(truthy("f_order_is_2"));     // 0, 1, 2 in turn: the last word is exit 2's
    CHECK(truthy("f_same"));           // one random draw, read twice
    CHECK(num("f_late") == 0 && num("f_pending") == 1);
    ticks(28);
    CHECK(run("f_late = triggers.counter('late')", "reading"));
    CHECK(num("f_late") == 0);         // CONTROL: not early
    ticks(3);
    CHECK(run("f_late = triggers.counter('late'); f_pending = graphs.pending()", "reading"));
    std::printf("  the delayed action ran once, after its 30 ticks: late=%d\n", (int)num("f_late"));
    CHECK(num("f_late") == 2 && num("f_pending") == 0);

    // ----------------------------------------------- a trigger condition
    std::printf("run: a trigger condition about the event, both ways\n");
    CHECK(load_graph(R"({
      "graph": 1, "name": "who",
      "nodes": [
        { "id": "touch", "type": "event",     "event": "on_touch" },
        { "id": "isp",   "type": "condition", "c": "entity_is_player" },
        { "id": "yes",   "type": "action",    "a": "counter_add", "counter": "player_touched" },
        { "id": "no",    "type": "action",    "a": "counter_add", "counter": "other_touched" }
      ],
      "links": [ ["touch.then", "isp.in"], ["isp.true", "yes.in"], ["isp.false", "no.in"] ]
    })"));
    CHECK(run("graphs.fire('on_touch', { entity = dnp.players()[1] })\n"
              "graphs.fire('on_touch', { entity = nil })\n"
              "w_yes = triggers.counter('player_touched'); w_no = triggers.counter('other_touched')", "touching"));
    CHECK(num("w_yes") == 1 && num("w_no") == 1);

    // --------------------------------------------------- bounded
    std::printf("bounded: a loop drawn by mistake stops, and the tick goes on\n");
    CHECK(load_graph(R"({
      "graph": 1, "name": "loop",
      "vars": { "spins": 0 },
      "nodes": [
        { "id": "t",    "type": "event",   "event": "on_tick" },
        { "id": "spin", "type": "add_var", "var": "spins", "value": 1 },
        { "id": "back", "type": "branch",  "cond": true }
      ],
      "links": [ ["t.then", "spin.in"], ["spin.then", "back.in"], ["back.true", "spin.in"] ]
    })"));
    ticks(2);
    CHECK(run("l_spins = graphs.var('loop', 'spins'); l_count = graphs.var('count', 'ticks')", "reading"));
    std::printf("  two ticks of an endless loop: %d steps of it, and the other graphs still ran\n",
                (int)num("l_spins"));
    CHECK(num("l_spins") > 100 && num("l_spins") <= 2 * 256);
    CHECK(num("l_count") == 25 + 1 + 28 + 3 + 2);   // CONTROL: every tick reached the counter graph

    // --------------------------------------------------- refused at load
    std::printf("refused: each of these is not loaded, with a reason\n");
    CHECK(run("before = graphs.count()", "counting"));
    const char* bad[] = {
        R"({"graph":1,"name":"b1","nodes":[{"id":"x","type":"teleport_everyone"}],"links":[]})",
        R"({"graph":1,"name":"b2","nodes":[{"id":"x","type":"once","colour":"red"}],"links":[]})",
        R"({"graph":1,"name":"b3","nodes":[{"id":"t","type":"event","event":"on_tick"}],"links":[["t.then","nobody.in"]]})",
        R"({"graph":1,"name":"b4","nodes":[{"id":"t","type":"event","event":"on_tick"},{"id":"a","type":"once"},{"id":"b","type":"once"}],"links":[["t.then","a.in"],["t.then","b.in"]]})",
        R"({"graph":1,"name":"b5","vars":{"v":0},"nodes":[{"id":"m","type":"math","op":"+"},{"id":"n","type":"math","op":"+"}],"links":[["m.value","n.a"],["n.value","m.a"]]})",
        R"({"graph":1,"name":"b6","nodes":[{"id":"t","type":"event","event":"on_tick"},{"id":"c","type":"action","a":"counter_add"}],"links":[["t.then","c.in"]]})",
        R"({"graph":1,"name":"b7","nodes":[{"id":"t","type":"event","event":"on_tick"},{"id":"s","type":"condition","c":"widget_is","widget":3}],"links":[["t.then","s.in"]]})",
        R"({"graph":1,"name":"b8","nodes":[{"id":"g","type":"get_var","var":"nope"}],"links":[]})",
        R"({"graph":2,"name":"b9","nodes":[],"links":[]})",
        R"({"graph":1,"name":"b10","nodes":[{"id":"t","type":"event","event":"on_tick"},{"id":"s","type":"set_var","var":"v"}],"links":[["t.then","s.value"]],"vars":{"v":0}})",
        R"({"graph":1,"name":"b11","nodes":[{"id":"t","type":"event","event":"on_teleport"}],"links":[]})",
    };
    int refused = 0;
    for (const char* b : bad) {
        if (!load_graph(b)) ++refused;
        else std::printf("  FAIL accepted: %s\n", b);
    }
    CHECK(run("after = graphs.count()", "counting"));
    std::printf("  %d of %zu bad graphs refused; %d loaded meanwhile\n", refused, sizeof(bad) / sizeof(bad[0]),
                (int)(num("after") - num("before")));
    CHECK(refused == static_cast<int>(sizeof(bad) / sizeof(bad[0])));
    CHECK(num("after") == num("before"));

    // ------------------------------------------------ the studio's copy
    // graph_vocab.json is what tools/vocabjson makes of graph_vocab.lua, byte for
    // byte: a stale copy offers the studio nodes the runtime refuses.
    {
        const auto slurp_cmd = [](const char* cmd) {
            std::string out;
            if (FILE* p = popen(cmd, "r")) {
                char buf[8192];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
                pclose(p);
            }
            return out;
        };
        const std::string fresh = slurp_cmd("./build/lua/trigger_vocab_json . maps/scripts/graph_vocab.lua graph_vocab 2>/dev/null");
        std::string held;
        if (FILE* f = std::fopen("maps/scripts/graph_vocab.json", "rb")) {
            char buf[8192];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) held.append(buf, n);
            std::fclose(f);
        }
        if (fresh.empty()) {
            std::printf("  ..   no build/lua/trigger_vocab_json; the JSON is not compared\n");
        } else {
            std::printf("json copy: graph_vocab.json is current\n");
            CHECK(fresh == held);
            // CONTROL: the comparison sees a change -- one node renamed differs.
            std::string changed = fresh;
            const size_t at = changed.find("\"branch\"");
            if (at != std::string::npos) changed.replace(at, 8, "\"brunch\"");
            CHECK(at != std::string::npos && changed != held);
        }
    }

    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
