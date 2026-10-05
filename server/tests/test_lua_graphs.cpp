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
#include <map>
#include <string>
#include <unistd.h>

#include "dnp/sim/world.hpp"
#include "dnp/geom/mapstore.hpp"
#include "sky.hpp"
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
    // The room (0), a DOOR east of it (1, shut: its ceiling on its floor, as
    // DOOM stores one) and a PLATFORM west of it (2), for the world's nodes.
    Room() {
        const fx2 p[] = {{-F(20), -F(20)}, {F(20), -F(20)}, {F(20), F(20)}, {-F(20), F(20)}};
        const fx2 d[] = {{F(20), -F(20)}, {F(24), -F(20)}, {F(24), F(20)}, {F(20), F(20)}};
        const fx2 w[] = {{-F(24), -F(20)}, {-F(20), -F(20)}, {-F(20), F(20)}, {-F(24), F(20)}};
        store.add_sector(p, 4, fx::zero(), F(10), 1);
        store.add_sector(d, 4, fx::zero(), fx::zero(), 2);
        store.add_sector(w, 4, fx::zero(), F(10), 3);
        for (u16 i = 0; i < 4; ++i)
            store.add_side(0, i, static_cast<u16>((i + 1) % 4), i == 1 ? 1 : (i == 3 ? 2 : geom::NO_SECTOR), 0);
        for (u16 i = 0; i < 4; ++i) store.add_side(1, i, static_cast<u16>((i + 1) % 4), i == 3 ? 0 : geom::NO_SECTOR, 0);
        for (u16 i = 0; i < 4; ++i) store.add_side(2, i, static_cast<u16>((i + 1) % 4), i == 1 ? 0 : geom::NO_SECTOR, 0);
        store.add_spawn("pad", 3, fx3{-F(22), F(5), fx::zero()});   // on the platform
        view = store.view();
    }
};

static lua_State* L = nullptr;
static server::lua::Vm vm;

// What the cinematic and camera nodes SEND, as the server would forward it to
// the client: camera and screen fields by wire id, HUD values by name.
struct RecordingUi : server::lua::UiSink {
    std::map<u8, i32> cams, scr;
    std::map<std::string, std::string> huds;
    u32  open_ui(u32, u32, const proto::UiPanel&) override { return 0; }
    void close_ui(u32, u32, u32) override {}
    void set_ui(u32, u32, u32, u16, i32) override {}
    void set_camera(u32, u32, const proto::CameraSet& c) override {
        for (u16 i = 0; i < c.count; ++i) cams[c.fields[i].id] = c.fields[i].milli;
    }
    void set_level_camera(u32, const proto::CameraSet&) override {}
    // The level's sky: -1 kind for "the module's again", as dnp.sky(nil) says.
    int sky_kind = -2;
    proto::SkySet sky{};
    void set_level_sky(u32, const proto::SkySet* s) override {
        sky_kind = s ? static_cast<int>(s->kind) : -1;
        if (s) sky = *s;
    }
    void set_side_texture(u32, const proto::SideTexture&) override {}
    std::vector<proto::SectorSet> sectors;
    void set_sector(u32, const proto::SectorSet& s) override { sectors.push_back(s); }
    void music(u32, i32, const proto::MusicCue&) override {}
    void screen(u32, i32, const proto::ScreenSet& sc) override {
        for (u16 i = 0; i < sc.count; ++i) scr[sc.fields[i].id] = sc.fields[i].milli;
    }
    void predict_sounds(u32, u32, u32, const proto::PredictSound*, u32) override {}
    bool hud(u32, i32, const std::vector<server::lua::HudSet>& set) override {
        for (const auto& h : set) huds[h.name] = h.kind == proto::HUD_CLEAR ? std::string("<cleared>") : h.s;
        return true;
    }
};
static RecordingUi g_ui;

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
static server::lua::CommandQueue* g_commands = nullptr;
static void ticks(int n) {
    for (int i = 0; i < n; ++i) {
        vm.call_hook("on_tick", g_world->tick());
        // The flush point, as the server's: what scripts asked for (a teleport,
        // a push) lands before the step.
        if (g_commands) g_commands->apply(*g_world, [](u32) {});
        g_world->step();
    }
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
    g_world = &world;

    static server::lua::CommandQueue commands;
    g_commands = &commands;
    static server::lua::EffectStack effects;
    static server::lua::WorldContext ctx;
    static i32 hp[sim::MAX_AGENTS];
    ctx.world = &world;
    ctx.map_store = &room.store;
    ctx.commands = &commands;
    ctx.effects = &effects;
    ctx.hp = hp;
    ctx.rng = Rng(20261003u, 5);
    ctx.ui = &g_ui;

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

    // ================================================================
    // EACH GRAPH HAS ITS OWN NAME: many graphs in one VM, but a second under
    // a name already loaded is refused. CONTROL: under a new name it loads.
    std::printf("-- each graph its own name --\n");
    CHECK(run("n_before = graphs.count()", "counting"));
    CHECK(!load_graph(R"({ "graph": 1, "name": "count", "nodes": [], "links": [] })"));
    CHECK(run("n_dup = graphs.count()", "counting"));
    CHECK(num("n_dup") == num("n_before"));
    CHECK(load_graph(R"({ "graph": 1, "name": "count_two", "nodes": [], "links": [] })"));
    CHECK(run("n_two = graphs.count()", "counting"));
    CHECK(num("n_two") == num("n_before") + 1);

    // ================================================================
    // DATA PINS ON VERB ARGUMENTS: a computed value reaches the world. `by` is
    // linked to 3 * 2; a condition's required `value` is linked, so the loader
    // must not ask for its literal; a fraction into an int is skipped, said.
    std::printf("-- verb arguments take data links --\n");
    CHECK(load_graph(R"({
      "graph": 1, "name": "pins", "vars": { "n": 3 },
      "nodes": [
        { "id": "go",   "type": "event", "event": "on_tick" },
        { "id": "one",  "type": "once" },
        { "id": "n",    "type": "get_var", "var": "n" },
        { "id": "two",  "type": "literal", "value": 2 },
        { "id": "six",  "type": "math", "op": "*" },
        { "id": "add",  "type": "action", "a": "counter_add", "counter": "pinned" },
        { "id": "half", "type": "math", "op": "/", "a": 1, "b": 2 },
        { "id": "bad",  "type": "action", "a": "counter_add", "counter": "pinned_bad" },
        { "id": "at",   "type": "condition", "c": "counter_at_least", "counter": "pinned" },
        { "id": "yes",  "type": "action", "a": "counter_add", "counter": "cond_yes" },
        { "id": "no",   "type": "action", "a": "counter_add", "counter": "cond_no" }
      ],
      "links": [
        ["go.then", "one.in"], ["one.then", "add.in"], ["add.then", "bad.in"], ["bad.then", "at.in"],
        ["at.true", "yes.in"], ["at.false", "no.in"],
        ["n.value", "six.a"], ["two.value", "six.b"], ["six.value", "add.by"],
        ["half.value", "bad.by"], ["six.value", "at.value"]
      ]
    })"));
    ticks(2);
    CHECK(run("p_pinned = triggers.counter('pinned'); p_bad = triggers.counter('pinned_bad');"
              "p_yes = triggers.counter('cond_yes'); p_no = triggers.counter('cond_no')", "reading"));
    std::printf("  pinned %g, misfit %g, condition yes %g no %g\n",
                num("p_pinned"), num("p_bad"), num("p_yes"), num("p_no"));
    CHECK(num("p_pinned") == 6);     // 3 * 2 reached counter_add's `by`
    CHECK(num("p_bad") == 0);        // 0.5 is not an int: skipped, not defaulted to 1
    CHECK(num("p_yes") == 1 && num("p_no") == 0);   // pinned 6 <= counter 6
    // CONTROL: a link into a pin the verb does not have is refused at load.
    CHECK(run("before_pin = graphs.count()", "counting"));
    CHECK(!load_graph(R"({
      "graph": 1, "name": "badpin",
      "nodes": [ { "id": "go", "type": "event", "event": "on_tick" },
                 { "id": "k", "type": "literal", "value": 1 },
                 { "id": "add", "type": "action", "a": "counter_add", "counter": "x" } ],
      "links": [ ["go.then", "add.in"], ["k.value", "add.nonsense"] ]
    })"));
    CHECK(run("after_pin = graphs.count()", "counting"));
    CHECK(num("after_pin") == num("before_pin"));

    // ================================================================
    // CALLS: a `call` starts the on_call nodes of its name, with what it sent;
    // one that calls itself stops at four deep.
    std::printf("-- calls --\n");
    CHECK(load_graph(R"({
      "graph": 1, "name": "calls", "vars": { "got": 0, "depth": 0 },
      "nodes": [
        { "id": "go",    "type": "event", "event": "on_tick" },
        { "id": "one",   "type": "once" },
        { "id": "ping",  "type": "call", "to": "level", "name": "ping", "value": 5 },
        { "id": "loop",  "type": "call", "to": "level", "name": "loop" },
        { "id": "hear",  "type": "on_call", "name": "ping" },
        { "id": "keep",  "type": "set_var", "var": "got" },
        { "id": "again", "type": "on_call", "name": "loop" },
        { "id": "deep",  "type": "add_var", "var": "depth", "value": 1 },
        { "id": "self",  "type": "call", "to": "level", "name": "loop" },
        { "id": "nobody","type": "call", "to": "level", "name": "nobody_listens" }
      ],
      "links": [
        ["go.then", "one.in"], ["one.then", "ping.in"], ["ping.then", "loop.in"], ["loop.then", "nobody.in"],
        ["hear.then", "keep.in"], ["hear.value", "keep.value"],
        ["again.then", "deep.in"], ["deep.then", "self.in"]
      ]
    })"));
    ticks(2);
    CHECK(run("c_got = graphs.var('calls', 'got'); c_depth = graphs.var('calls', 'depth')", "reading"));
    std::printf("  ping carried %g; a self-call ran %g deep\n", num("c_got"), num("c_depth"));
    CHECK(num("c_got") == 5);
    CHECK(num("c_depth") == 4);      // bounded, and it did recurse (CONTROL: > 1)

    // ================================================================
    // SAVED WITH THE GAME: variables and passed `once` nodes, by graph file.
    std::printf("-- graphs persist --\n");
    const char* keep_json = R"({
      "graph": 1, "name": "keep", "vars": { "score": 1, "label": "start" },
      "nodes": [
        { "id": "go",   "type": "event", "event": "on_tick" },
        { "id": "one",  "type": "once" },
        { "id": "set",  "type": "set_var", "var": "score", "value": 9 },
        { "id": "mark", "type": "action", "a": "counter_add", "counter": "keep_once" }
      ],
      "links": [ ["go.then", "one.in"], ["one.then", "set.in"], ["set.then", "mark.in"] ]
    })";
    CHECK(load_graph(keep_json));
    ticks(2);
    std::string saved, perr;
    CHECK(server::lua::persist_capture(L, saved, perr));
    // A fresh start: the graphs forgotten and loaded again from their files.
    CHECK(run("graphs.reset()", "reset"));
    CHECK(load_graph(keep_json));
    CHECK(run("k_fresh = graphs.var('keep', 'score')", "reading"));
    CHECK(num("k_fresh") == 1);      // CONTROL: a fresh load really starts over
    CHECK(server::lua::persist_restore(L, saved, perr));
    CHECK(run("k_score = graphs.var('keep', 'score'); k_once = triggers.counter('keep_once')", "reading"));
    ticks(2);
    CHECK(run("k_once2 = triggers.counter('keep_once')", "reading"));
    std::printf("  restored score %g; the once ran %g time(s) before and %g after\n",
                num("k_score"), num("k_once"), num("k_once2"));
    CHECK(num("k_score") == 9);
    CHECK(num("k_once2") == num("k_once"));   // its `once` stays passed
    // A variable the graph now holds as another type is dropped, not forced in.
    CHECK(run("graphs.reset()", "reset"));
    CHECK(load_graph(R"({ "graph": 1, "name": "keep", "vars": { "score": "nine" },
                          "nodes": [], "links": [] })"));
    CHECK(server::lua::persist_restore(L, saved, perr));
    CHECK(run("k_type = graphs.var('keep', 'score') == 'nine'", "reading"));
    CHECK(truthy("k_type"));

    // ================================================================
    // THE WORLD'S NODES: doors, floors, entities, by what they are -- no
    // indices beyond the one a map pick would fill in.
    std::printf("-- the world's nodes --\n");
    CHECK(run("w_shut = dnp.sector(1):ceil().raw", "the door before"));
    CHECK(num("w_shut") == 0);
    CHECK(load_graph(R"({
      "graph": 1, "name": "world", "vars": { "px": 0, "py": 0, "where": -1 },
      "nodes": [
        { "id": "go",    "type": "event", "event": "on_tick" },
        { "id": "one",   "type": "once" },
        { "id": "door",  "type": "open_door", "sector": 1, "speed": "fast" },
        { "id": "up",    "type": "move_floor", "sector": 2, "to": "up_by", "amount": 2, "speed": "fast" },
        { "id": "tp",    "type": "teleport", "x": 5, "y": 6 },
        { "id": "pos",   "type": "position_of" },
        { "id": "keepx", "type": "set_var", "var": "px" },
        { "id": "keepy", "type": "set_var", "var": "py" },
        { "id": "sec",   "type": "sector_of" },
        { "id": "keeps", "type": "set_var", "var": "where" },
        { "id": "shove", "type": "push", "direction": "angle", "angle": 0, "strength": 4 }
      ],
      "links": [
        ["go.then", "one.in"], ["one.then", "door.in"], ["door.then", "up.in"], ["up.then", "tp.in"],
        ["tp.then", "shove.in"], ["shove.then", "keepx.in"], ["keepx.then", "keepy.in"], ["keepy.then", "keeps.in"],
        ["pos.x", "keepx.value"], ["pos.y", "keepy.value"], ["sec.sector", "keeps.value"]
      ]
    })"));
    CHECK(run("graphs.fire('on_tick', { entity = dnp.players()[1] })", "the run, with a player"));
    ticks(1);
    CHECK(run("w_vx = dnp.players()[1]:vel().x.raw / 4294967296", "the push"));
    ticks(239);
    CHECK(run("w_door = dnp.sector(1):ceil().raw / 4294967296\n"
              "w_floor = dnp.sector(2):floor().raw / 4294967296\n"
              "local p = dnp.players()[1]:pos(); w_x = p.x.raw / 4294967296; w_y = p.y.raw / 4294967296\n"
              "w_px = graphs.var('world', 'px'); w_where = graphs.var('world', 'where')", "reading the world"));
    std::printf("  door ceiling %.3f, platform floor %.3f, player at %.2f,%.2f (read back %.2f), in sector %g\n",
                num("w_door"), num("w_floor"), num("w_x"), num("w_y"), num("w_px"), num("w_where"));
    CHECK(num("w_door") > 9.9 && num("w_door") < 10);   // the room's ceiling, less DOOM's four units
    CHECK(num("w_floor") == 2);                          // up by exactly 2
    CHECK(num("w_x") > 5 && num("w_x") < 6 && num("w_y") == 6);   // teleported to 5,6, then pushed east
    CHECK(num("w_px") == 0);                             // read in the same run: before the flush, as documented
    CHECK(num("w_vx") > 0);                              // and the push, east, moved it
    CHECK(num("w_where") == 0);                          // standing in the room
    // CONTROL: a door told to close after a second shuts again; the first stays.
    CHECK(load_graph(R"({
      "graph": 1, "name": "shuts",
      "nodes": [ { "id": "go", "type": "event", "event": "on_tick" }, { "id": "one", "type": "once" },
                 { "id": "lift", "type": "lift", "sector": 2, "wait": 1, "speed": "fast" },
                 { "id": "door", "type": "open_door", "sector": 1, "speed": "fast", "close_after": 1 } ],
      "links": [ ["go.then", "one.in"], ["one.then", "door.in"], ["door.then", "lift.in"] ]
    })"));
    ticks(30);
    CHECK(run("s_low = dnp.sector(2):floor().raw / 4294967296", "the lift going down"));
    ticks(400);
    CHECK(run("s_door = dnp.sector(1):ceil().raw / 4294967296; s_back = dnp.sector(2):floor().raw / 4294967296", "after"));
    std::printf("  close_after: door ceiling %.3f; the lift went down to %.2f and came back to %.2f\n",
                num("s_door"), num("s_low"), num("s_back"));
    CHECK(num("s_door") == 0);
    CHECK(num("s_low") < 2);                 // down toward the lowest floor beside it (the room's 0)
    CHECK(num("s_back") == 0);               // and back to where it was authored
    // A sector node with nothing to act on is refused; so is a sector the map lacks.
    CHECK(!load_graph(R"({ "graph": 1, "name": "nosec",
      "nodes": [ { "id": "go", "type": "event", "event": "on_tick" }, { "id": "d", "type": "open_door" } ],
      "links": [ ["go.then", "d.in"] ] })"));
    CHECK(!load_graph(R"({ "graph": 1, "name": "nosuch",
      "nodes": [ { "id": "go", "type": "event", "event": "on_tick" }, { "id": "d", "type": "open_door", "sector": 99 } ],
      "links": [ ["go.then", "d.in"] ] })"));

    // ================================================================
    // WHEN THINGS HAPPEN: start nodes that fire once for the moment they name.
    std::printf("-- when things happen --\n");
    CHECK(run("graphs.reset(); dnp.players()[1]:teleport(vec3.new(fx.int(0), fx.int(0), fx.int(0)))", "a clean slate"));
    ticks(2);
    CHECK(load_graph(R"({
      "graph": 1, "name": "when",
      "nodes": [
        { "id": "start", "type": "when_level_starts" },
        { "id": "c0",    "type": "action", "a": "counter_add", "counter": "w_started" },
        { "id": "ent",   "type": "when_sector_entered", "sector": 2 },
        { "id": "c1",    "type": "action", "a": "counter_add", "counter": "w_entered" },
        { "id": "door",  "type": "when_sector_entered", "sector": 1 },
        { "id": "c2",    "type": "action", "a": "counter_add", "counter": "w_door" },
        { "id": "left",  "type": "when_sector_left", "sector": 0 },
        { "id": "c3",    "type": "action", "a": "counter_add", "counter": "w_left" },
        { "id": "plate", "type": "when_floor_touched", "sector": 2, "repeat_every": 0.5 },
        { "id": "c4",    "type": "action", "a": "counter_add", "counter": "w_floor" },
        { "id": "near",  "type": "when_near", "x": -22, "y": 0, "radius": 1 },
        { "id": "c5",    "type": "action", "a": "counter_add", "counter": "w_near" },
        { "id": "c6",    "type": "action", "a": "counter_add", "counter": "w_away" },
        { "id": "tick",  "type": "every", "seconds": 0.5 },
        { "id": "c7",    "type": "action", "a": "counter_add", "counter": "w_every" },
        { "id": "once",  "type": "after", "seconds": 1 },
        { "id": "c8",    "type": "action", "a": "counter_add", "counter": "w_after" },
        { "id": "touch", "type": "when_prop_touched", "prop": "medikit" },
        { "id": "c9",    "type": "action", "a": "counter_add", "counter": "w_touch" }
      ],
      "links": [ ["start.then", "c0.in"], ["ent.then", "c1.in"], ["door.then", "c2.in"], ["left.then", "c3.in"],
                 ["plate.then", "c4.in"], ["near.near", "c5.in"], ["near.away", "c6.in"],
                 ["tick.then", "c7.in"], ["once.then", "c8.in"], ["touch.then", "c9.in"] ]
    })"));
    ticks(5);
    // Onto the platform: out of the room, into sector 2, onto its floor, near the spot.
    CHECK(run("dnp.players()[1]:teleport(vec3.new(fx.int(-22), fx.int(0), fx.int(0)))", "onto the platform"));
    ticks(95);                                         // 100 ticks since loading
    // A touch held for three ticks is one touch; after a gap, another.
    CHECK(run("local p = dnp.players()[1]\n"
              "local kit = { index = function() return 4321 end, name = function() return 'medikit' end }\n"
              "local now = dnp.tick()\n"
              "graphs.touched(p, kit)", "touch 1"));
    ticks(1); CHECK(run("graphs.touched(dnp.players()[1], { index = function() return 4321 end, name = function() return 'medikit' end })", "held")); 
    ticks(1); CHECK(run("graphs.touched(dnp.players()[1], { index = function() return 4321 end, name = function() return 'medikit' end })", "held"));
    ticks(5); CHECK(run("graphs.touched(dnp.players()[1], { index = function() return 4321 end, name = function() return 'medikit' end })", "again"));
    CHECK(run("dnp.players()[1]:teleport(vec3.new(fx.int(0), fx.int(0), fx.int(0)))", "back to the room"));
    ticks(3);
    CHECK(run("local c = triggers.counter\n"
              "w = { c('w_started'), c('w_entered'), c('w_door'), c('w_left'), c('w_floor'), c('w_near'), c('w_away'),"
              "      c('w_every'), c('w_after'), c('w_touch') }", "reading"));
    CHECK(run("w1, w2, w3, w4, w5, w6, w7, w8, w9, w10 = unpack(w)", "unpacking"));
    std::printf("  started %g, entered %g (door %g), left %g, floor %g, near %g away %g, every %g, after %g, touched %g\n",
                num("w1"), num("w2"), num("w3"), num("w4"), num("w5"), num("w6"), num("w7"), num("w8"), num("w9"), num("w10"));
    CHECK(num("w1") == 1);                      // once
    CHECK(num("w2") == 1 && num("w4") == 1);    // into the platform, out of the room
    CHECK(num("w3") == 0);                      // CONTROL: not the door
    CHECK(num("w5") >= 3);                      // landed, then again every half second while on it
    CHECK(num("w6") == 1 && num("w7") == 1);    // came near once, went away once
    CHECK(num("w8") >= 3 && num("w8") <= 4);    // ~110 ticks at every 30
    CHECK(num("w9") == 1);                      // after a second, once
    CHECK(num("w10") == 2);                     // held touches are one; a later one is another

    // ================================================================
    // A CUTSCENE: controls taken, bars in, two lines spoken one after another,
    // a fade, the camera told what to do -- and all of it given back.
    std::printf("-- a cutscene --\n");
    CHECK(run("graphs.reset()", "a clean slate"));
    CHECK(load_graph(R"({
      "graph": 1, "name": "scene",
      "nodes": [
        { "id": "go",    "type": "when_level_starts" },
        { "id": "start", "type": "start_cinematic", "everyone": true },
        { "id": "look",  "type": "camera_follow", "everyone": true, "distance": 12, "height": 30, "lock": true },
        { "id": "line1", "type": "subtitle", "everyone": true, "text": "Where is everybody?", "speaker": "Marine", "seconds": 1 },
        { "id": "line2", "type": "subtitle", "everyone": true, "text": "Too quiet.", "seconds": 1 },
        { "id": "dark",  "type": "fade", "everyone": true, "to": "black", "seconds": 0.5 },
        { "id": "boom",  "type": "camera_shake", "everyone": true, "strength": 0.5 },
        { "id": "stop",  "type": "end_cinematic", "everyone": true },
        { "id": "mark",  "type": "action", "a": "counter_add", "counter": "scene_done" }
      ],
      "links": [ ["go.then", "start.in"], ["start.then", "look.in"], ["look.then", "line1.in"],
                 ["line1.done", "line2.in"], ["line2.done", "dark.in"], ["dark.done", "boom.in"],
                 ["boom.then", "stop.in"], ["stop.then", "mark.in"] ]
    })"));
    ticks(5);
    // The rules as the server applies them: the effect stack over the base.
    const physics::MoveParams during = effects.compute(player);
    const i32 bars = g_ui.scr.count(2) ? g_ui.scr[2] : -1;
    const std::string first = g_ui.huds["subtitle"], who = g_ui.huds["subtitle_speaker"];
    const i32 pitch = g_ui.cams.count(1) ? g_ui.cams[1] : -1;
    ticks(70);                                   // the first line's second is over
    const std::string second = g_ui.huds["subtitle"];
    ticks(120);                                  // the second line, the fade, the shake, the end
    CHECK(run("cs_done = triggers.counter('scene_done')", "after"));
    const physics::MoveParams after = effects.compute(player);
    std::printf("  frozen: input_scale %g; bars %d; said \"%s\" (%s), then \"%s\"; pitch %d; after: %g, bars %d, cam_mode %d, shake %d, done %g\n",
                during.input_scale.raw / 4294967296.0, bars, first.c_str(), who.c_str(), second.c_str(), pitch,
                after.input_scale.raw / 4294967296.0,
                g_ui.scr[2], g_ui.cams.count(11) ? g_ui.cams[11] : -1, g_ui.cams.count(16) ? g_ui.cams[16] : -1,
                num("cs_done"));
    CHECK(during.input_scale == fx::zero() && !during.attack_enabled);   // the controls taken: no moving, no attacking
    CHECK(bars == 120);                          // the bars, 0.12 of the screen
    CHECK(first == "Where is everybody?" && who == "Marine");
    CHECK(pitch == 83);                          // 30 degrees, in thousandths of a turn
    CHECK(second == "Too quiet.");               // the second line, after the first's `done`
    CHECK(g_ui.scr[0] == 1000);               // faded to black
    CHECK(g_ui.cams[16] == 500);               // shaken
    CHECK(after.input_scale == fx::one() && after.attack_enabled);       // the controls given back
    CHECK(g_ui.scr[2] == 0);                  // the bars away
    CHECK(g_ui.cams[11] == 0);                 // the camera back to following
    CHECK(g_ui.huds["subtitle"] == "<cleared>");
    CHECK(num("cs_done") == 1);
    // A path the map does not have is refused, by tag.
    CHECK(!load_graph(R"({ "graph": 1, "name": "fly",
      "nodes": [ { "id": "go", "type": "when_level_starts" }, { "id": "f", "type": "camera_flythrough", "path": 7 } ],
      "links": [ ["go.then", "f.in"] ] })"));

    // ================================================================
    // A SPAWN POINT BY ITS TAG: teleport there; a tag the map lacks is refused.
    std::printf("-- spawn points --\n");
    CHECK(load_graph(R"({ "graph": 1, "name": "to_pad",
      "nodes": [ { "id": "go", "type": "when_level_starts" },
                 { "id": "t", "type": "teleport", "spawn": "pad", "entity_from": null } ],
      "links": [ ["go.then", "t.in"] ] })") == false);   // an unknown field: refused, as any is
    CHECK(load_graph(R"({ "graph": 1, "name": "to_pad",
      "nodes": [ { "id": "go", "type": "on_call", "name": "pad" }, { "id": "t", "type": "teleport", "spawn": "pad" } ],
      "links": [ ["go.then", "t.in"] ] })"));
    CHECK(run("graphs.call('pad', dnp.players()[1], nil)", "the call"));
    ticks(2);
    CHECK(run("local p = dnp.players()[1]:pos(); sp_x = p.x.raw / 4294967296; sp_y = p.y.raw / 4294967296", "where"));
    std::printf("  teleported to spawn 'pad': %.2f, %.2f\n", num("sp_x"), num("sp_y"));
    CHECK(num("sp_x") == -22 && num("sp_y") == 5);
    CHECK(!load_graph(R"({ "graph": 1, "name": "to_nowhere",
      "nodes": [ { "id": "go", "type": "on_call", "name": "x" }, { "id": "t", "type": "teleport", "spawn": "nope" } ],
      "links": [ ["go.then", "t.in"] ] })"));

    std::printf("-- the sky (docs/SKY.md) --\n");
    {
        // The words a module.txt `sky` line and dnp.sky both take.
        proto::SkySet s;
        std::string err;
        CHECK(server::parse_sky(server::sky_words("strip SKY2 repeat 4 yaw 0.25"), s, err));
        CHECK(s.kind == proto::SkyKind::Strip && s.images == 1 && s.count == 2 &&
              s.fields[0].id == server::SKY_FIELD_REPEAT && s.fields[0].milli == 4000 && s.fields[1].milli == 250);
        CHECK(server::parse_sky(server::sky_words("cube a b c d e f"), s, err) && s.images == 6);
        // CONTROLS: each is refused, saying why.
        for (const char* bad : {"cube a b c", "strip", "dome SKY1", "strip ../x", "panorama P repeat 2", "strip S repeat 0",
                                "strip S yaw"}) {
            err.clear();
            const bool ok = server::parse_sky(server::sky_words(bad), s, err);
            if (ok || err.empty()) std::printf("  accepted '%s'\n", bad);
            CHECK(!ok && !err.empty());
        }
        // Which level gets which: its own name, then dw_map12's map12, then default.
        std::vector<server::SkyEntry> skies;
        proto::SkySet one, two;
        server::parse_sky(server::sky_words("strip SKY1"), one, err);
        server::parse_sky(server::sky_words("strip SKY2"), two, err);
        skies.push_back({"default", one});
        skies.push_back({"map12", two});
        CHECK(server::sky_for(skies, "map12") == &skies[1].sky);
        CHECK(server::sky_for(skies, "dw_map12") == &skies[1].sky);
        CHECK(server::sky_for(skies, "map01") == &skies[0].sky);
        CHECK(server::sky_for({}, "map01") == nullptr);
        // A level's saved state keeps a script's sky as words: read back, the same sky.
        for (const char* w : {"strip SKY2 repeat 2.5 yaw -0.25 height 0.7", "cube a b c d e f yaw 0.125", "none"}) {
            proto::SkySet a, b;
            CHECK(server::parse_sky(server::sky_words(w), a, err));
            const std::string text = server::sky_text(a);
            CHECK(server::parse_sky(server::sky_words(text), b, err));
            // Field by field: CameraField has padding, which memcmp would read.
            bool same = a.kind == b.kind && a.images == b.images && a.count == b.count;
            for (u16 i = 0; same && i < a.count; ++i)
                same = a.fields[i].id == b.fields[i].id && a.fields[i].milli == b.fields[i].milli;
            for (u8 i = 0; same && i < a.images; ++i)
                same = a.image[i].len == b.image[i].len && std::memcmp(a.image[i].text, b.image[i].text, a.image[i].len) == 0;
            if (!same) std::printf("  '%s' came back as '%s'\n", w, text.c_str());
            CHECK(same);
        }
    }
    CHECK(run("dnp.sky('panorama dusk yaw 0.5')", "dnp.sky words"));
    CHECK(g_ui.sky_kind == static_cast<int>(proto::SkyKind::Panorama) && g_ui.sky.image[0].len == 4);
    CHECK(run("dnp.sky{ kind = 'cube', faces = { 'a', 'b', 'c', 'd', 'e', 'f' } }", "dnp.sky table"));
    CHECK(g_ui.sky_kind == static_cast<int>(proto::SkyKind::Cube) && g_ui.sky.images == 6);
    CHECK(run("dnp.sky(nil)", "dnp.sky nil"));
    CHECK(g_ui.sky_kind == -1);
    CHECK(run("sky_ok, sky_err = pcall(dnp.sky, 'strip')", "dnp.sky refused"));
    CHECK(!truthy("sky_ok"));
    // The graph node: degrees in, turns on the wire; `default` is dnp.sky(nil).
    CHECK(load_graph(R"({ "graph": 1, "name": "skies",
      "nodes": [ { "id": "go", "type": "on_call", "name": "dusk" },
                 { "id": "s", "type": "set_sky", "kind": "strip", "image": "SKY3", "repeat": 2, "yaw": 90 },
                 { "id": "back", "type": "on_call", "name": "back" },
                 { "id": "d", "type": "set_sky", "kind": "default" } ],
      "links": [ ["go.then", "s.in"], ["back.then", "d.in"] ] })"));
    CHECK(run("graphs.call('dusk', nil, nil)", "set_sky"));
    std::printf("  set_sky: kind %d, picture %.*s, repeat %d, yaw %d\n", g_ui.sky_kind,
                static_cast<int>(g_ui.sky.image[0].len), g_ui.sky.image[0].text, g_ui.sky.fields[0].milli, g_ui.sky.fields[1].milli);
    CHECK(g_ui.sky_kind == static_cast<int>(proto::SkyKind::Strip) && g_ui.sky.fields[0].milli == 2000 &&
          g_ui.sky.fields[1].milli == 250);
    CHECK(run("graphs.call('back', nil, nil)", "set_sky default"));
    CHECK(g_ui.sky_kind == -1);

    std::printf("-- sector looks (SECTOR_SET) --\n");
    {
        g_ui.sectors.clear();
        CHECK(run("dnp.set_sector_texture(2, 'floor', 'LAVA1')", "floor"));
        CHECK(run("dnp.set_sector_light(0, 40, 2.5)", "light"));
        CHECK(run("dnp.set_sector_effect(0, 'fog', { density = 0.5, color = '#203040' })", "fog"));
        CHECK(run("dnp.set_sector_effect(0, 'rain', false)", "rain off"));
        CHECK(g_ui.sectors.size() == 4);
        if (g_ui.sectors.size() == 4) {
            const auto& f = g_ui.sectors[0];
            CHECK(f.sector == 2 && f.what == proto::SectorWhat::Floor && f.texture.len == 5);
            const auto& l = g_ui.sectors[1];
            CHECK(l.what == proto::SectorWhat::Light && l.light == 40 && l.fade_ms == 2500);
            const auto& g = g_ui.sectors[2];
            CHECK(g.what == proto::SectorWhat::Effect && g.on && g.fx.kind == 5 && g.fx.count == 2);
            bool colour = false, dense = false;
            for (u8 i = 0; i < g.fx.count; ++i) {
                colour = colour || (g.fx.params[i].key == 8 && g.fx.params[i].raw == 0x203040);
                dense = dense || (g.fx.params[i].key == 1 && g.fx.params[i].raw == fx::from_frac(1, 2).raw);
            }
            CHECK(colour && dense);
            CHECK(!g_ui.sectors[3].on && g_ui.sectors[3].fx.kind == 1);
        }
        // CONTROLS: a sector the map lacks, an effect that is not one, and a
        // setting fog does not take are each a script error, said.
        for (const char* bad : {"dnp.set_sector_light(9, 10)", "dnp.set_sector_effect(0, 'smoke')",
                                "dnp.set_sector_effect(0, 'fog', { speed = 3 })",
                                "dnp.set_sector_texture(0, 'wall', 'X')"}) {
            const std::string src = std::string("sx_ok = pcall(function() ") + bad + " end)";
            CHECK(run(src.c_str(), bad));
            if (truthy("sx_ok")) std::printf("  accepted: %s\n", bad);
            CHECK(!truthy("sx_ok"));
        }
        // The graph nodes, by tag: light fading, ceiling opened to the sky, snow.
        g_ui.sectors.clear();
        CHECK(load_graph(R"({ "graph": 1, "name": "looks",
          "nodes": [ { "id": "go", "type": "on_call", "name": "dusk" },
                     { "id": "l", "type": "set_sector_light", "tag": 1, "light": 60, "seconds": 3 },
                     { "id": "c", "type": "set_ceiling_texture", "tag": 3, "texture": "F_SKY1" },
                     { "id": "s", "type": "set_sector_effect", "tag": 1, "effect": "snow", "density": 2 },
                     { "id": "o", "type": "set_sector_effect", "tag": 3, "effect": "fog", "on": false } ],
          "links": [ ["go.then", "l.in"], ["l.then", "c.in"], ["c.then", "s.in"], ["s.then", "o.in"] ] })"));
        CHECK(run("graphs.call('dusk', nil, nil)", "the looks"));
        CHECK(g_ui.sectors.size() == 4);
        if (g_ui.sectors.size() == 4) {
            CHECK(g_ui.sectors[0].sector == 0 && g_ui.sectors[0].light == 60 && g_ui.sectors[0].fade_ms == 3000);
            CHECK(g_ui.sectors[1].sector == 2 && g_ui.sectors[1].what == proto::SectorWhat::Ceiling);
            CHECK(g_ui.sectors[2].fx.kind == 2 && g_ui.sectors[2].on && g_ui.sectors[2].fx.params[0].raw == F(2).raw);
            CHECK(g_ui.sectors[3].fx.kind == 5 && !g_ui.sectors[3].on);
        }
        // CONTROL: a tag no sector carries changes nothing.
        g_ui.sectors.clear();
        CHECK(load_graph(R"({ "graph": 1, "name": "nowhere",
          "nodes": [ { "id": "go", "type": "on_call", "name": "nowhere" },
                     { "id": "l", "type": "set_sector_light", "tag": 77, "light": 0 } ],
          "links": [ ["go.then", "l.in"] ] })"));
        CHECK(run("graphs.call('nowhere', nil, nil)", "nowhere") && g_ui.sectors.empty());
    }

    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
