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
static fx Ff(i64 a, i64 b) { return fx::from_frac(a, b); }

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
        // Three EFFECT sectors laid over the room: 3 (x -10..0) and 4 (x -5..5)
        // overlapping each other, and 5 over all of it, above a body's head.
        const fx2 e3[] = {{-F(10), -F(10)}, {F(0), -F(10)}, {F(0), F(10)}, {-F(10), F(10)}};
        const fx2 e4[] = {{-F(5), -F(10)}, {F(5), -F(10)}, {F(5), F(10)}, {-F(5), F(10)}};
        const fx2 e5[] = {{-F(10), -F(10)}, {F(10), -F(10)}, {F(10), F(10)}, {-F(10), F(10)}};
        store.add_sector(e3, 4, fx::zero(), F(10), 7);
        store.add_sector(e4, 4, fx::zero(), F(10), 8);
        store.add_sector(e5, 4, F(6), F(10), 9);
        for (u32 s = 3; s <= 5; ++s) {
            for (u16 i = 0; i < 4; ++i) store.add_side(s, i, static_cast<u16>((i + 1) % 4), geom::NO_SECTOR, 0);
            store.sector_mut(s).effect = true;
        }
        store.rebuild_grid();
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
    // Panels: each open gets the next id, as the server's do.
    std::vector<proto::UiPanel> opened;
    std::vector<u32> closed_ids;
    struct Set { u32 ui; u16 widget; i32 value; };
    std::vector<Set> sets;
    u32  next_ui = 100;
    u32  open_ui(u32, u32, const proto::UiPanel& p) override { opened.push_back(p); return ++next_ui; }
    void close_ui(u32, u32, u32 id) override { closed_ids.push_back(id); }
    void set_ui(u32, u32, u32 id, u16 w, i32 v) override { sets.push_back({id, w, v}); }
    struct Show { u32 ui; u16 widget; std::string decor; bool shown; };
    std::vector<Show> shows;
    void show_ui(u32, u32, u32 id, u16 w, const std::string& d, bool shown) override { shows.push_back({id, w, d, shown}); }
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
        for (const auto& h : set) {
            huds[h.name] = h.kind == proto::HUD_CLEAR ? std::string("<cleared>")
                         : h.kind == proto::HUD_INT  ? std::to_string(h.i) : h.s;
        }
        return true;
    }
    // MARKERS, as the server keeps them: by id, whole, with who sees them.
    struct Mark { proto::MarkSet m; i32 to; };
    std::map<u32, Mark> marks;
    u32 next_mark = 500;
    int unmarked = 0;
    u32 mark(u32, const proto::MarkSet& m, i32 to) override {
        const u32 id = ++next_mark;
        marks[id] = Mark{m, to};
        marks[id].m.id = id;
        return id;
    }
    bool get_mark(u32, u32 id, proto::MarkSet& out) override {
        const auto it = marks.find(id);
        if (it == marks.end()) return false;
        out = it->second.m;
        return true;
    }
    bool remark(u32, const proto::MarkSet& m) override {
        const auto it = marks.find(m.id);
        if (it == marks.end()) return false;
        it->second.m = m;
        return true;
    }
    void unmark(u32, u32 id) override { unmarked += static_cast<int>(marks.erase(id)); }
    // A marker's value by name: its number, its text, or "<none>".
    std::string value(u32 id, const char* name) const {
        const auto it = marks.find(id);
        if (it == marks.end()) return "<no marker>";
        const proto::MarkSet& m = it->second.m;
        for (u8 i = 0; i < m.count; ++i) {
            const proto::MarkValue& v = m.values[i];
            if (std::string(v.name, v.name_len) != name) continue;
            return v.kind == proto::HUD_STR ? std::string(v.text.text, v.text.len) : std::to_string(v.number);
        }
        return "<none>";
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
// Whether `src` raises -- for a refusal that is the point of the check.
static bool raises(const char* src) {
    if (luaL_dostring(L, src) == 0) return false;
    lua_pop(L, 1);
    return true;
}
static double num(const char* global) {
    lua_getglobal(L, global);
    const double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1;
    lua_pop(L, 1);
    return v;
}
static std::string str(const char* global) {
    lua_getglobal(L, global);
    const std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
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
    static u16 avatar[sim::MAX_AGENTS];
    static proto::Look look[sim::MAX_AGENTS];
    static u16 look_rev[sim::MAX_AGENTS];
    ctx.avatar = avatar;
    ctx.look = look;
    ctx.look_rev = look_rev;
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
    // EFFECT SECTORS: entering and leaving each of two that overlap, while the
    // body's own sector stays the room's.
    std::printf("-- effect sectors --\n");
    CHECK(run("graphs.reset(); dnp.players()[1]:teleport(vec3.new(fx.int(15), fx.int(0), fx.int(0)))", "a clean slate"));
    ticks(2);
    CHECK(load_graph(R"({
      "graph": 1, "name": "volumes",
      "nodes": [
        { "id": "in3",  "type": "when_sector_entered", "sector": 3 },
        { "id": "c1",   "type": "action", "a": "counter_add", "counter": "v_in3" },
        { "id": "out3", "type": "when_sector_left", "sector": 3 },
        { "id": "c2",   "type": "action", "a": "counter_add", "counter": "v_out3" },
        { "id": "in4",  "type": "when_sector_entered", "tag": 8 },
        { "id": "c3",   "type": "action", "a": "counter_add", "counter": "v_in4" },
        { "id": "out4", "type": "when_sector_left", "sector": 4 },
        { "id": "c4",   "type": "action", "a": "counter_add", "counter": "v_out4" },
        { "id": "in5",  "type": "when_sector_entered", "sector": 5 },
        { "id": "c5",   "type": "action", "a": "counter_add", "counter": "v_in5" },
        { "id": "room", "type": "when_sector_left", "sector": 0 },
        { "id": "c6",   "type": "action", "a": "counter_add", "counter": "v_room" }
      ],
      "links": [ ["in3.then", "c1.in"], ["out3.then", "c2.in"], ["in4.then", "c3.in"], ["out4.then", "c4.in"],
                 ["in5.then", "c5.in"], ["room.then", "c6.in"] ]
    })"));
    ticks(3);
    const auto at = [&](int x, const char* what) {
        CHECK(run((std::string("dnp.players()[1]:teleport(vec3.new(fx.int(") + std::to_string(x) +
                   "), fx.int(0), fx.int(0)))").c_str(), what));
        ticks(3);
    };
    at(-7, "into 3");
    at(-2, "into 4 as well");
    CHECK(run("local p = dnp.players()[1]; v_both = table.concat(p:effect_sectors(), ','); v_sec = p:sector()", "reading"));
    at(3, "out of 3, still in 4");
    at(15, "out of 4");
    CHECK(run("local c = triggers.counter\n"
              "v1, v2, v3, v4, v5, v6 = c('v_in3'), c('v_out3'), c('v_in4'), c('v_out4'), c('v_in5'), c('v_room')", "counting"));
    std::printf("  in both at x -2: {%s}, its sector %g; 3 in %g out %g, 4 in %g out %g; 5 (over the head) %g; the room left %g\n",
                str("v_both").c_str(), num("v_sec"), num("v1"), num("v2"), num("v3"), num("v4"), num("v5"), num("v6"));
    CHECK(str("v_both") == "3,4" && num("v_sec") == 0);
    CHECK(num("v1") == 1 && num("v2") == 1 && num("v3") == 1 && num("v4") == 1);
    CHECK(num("v5") == 0);                      // CONTROL: its height is above the body's head
    CHECK(num("v6") == 0);                      // CONTROL: the room was never left

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
    // A LONG LINE reaches the HUD whole: five lines at a big size is ~200
    // letters, and the old cap of 128 bytes kept three. One past the cap is
    // cut at a whole letter (a two-byte "é" across the edge goes entirely).
    CHECK(run("dnp.hud(nil, { long_line = string.rep('word ', 60) })\n"
              "dnp.hud(nil, { cut_line = string.rep('a', 1023) .. '\\195\\169' })", "a long line"));
    std::printf("  a 300-byte line arrives as %zu bytes; one with a letter across the cap as %zu\n",
                g_ui.huds["long_line"].size(), g_ui.huds["cut_line"].size());
    CHECK(g_ui.huds["long_line"].size() == 300);
    CHECK(g_ui.huds["cut_line"].size() == 1023);
    // A path the map does not have is refused, by tag.
    CHECK(!load_graph(R"({ "graph": 1, "name": "fly",
      "nodes": [ { "id": "go", "type": "when_level_starts" }, { "id": "f", "type": "camera_flythrough", "path": 7 } ],
      "links": [ ["go.then", "f.in"] ] })"));

    // ================================================================
    // QUIT: a main menu's "Quit" asks the player's client to close the app
    // (SCREEN_SET field 4). CONTROL: nothing asked it before the node ran.
    std::printf("-- quit the game --\n");
    g_ui.scr.erase(4);
    CHECK(load_graph(R"({ "graph": 1, "name": "menu_quit",
      "nodes": [ { "id": "go", "type": "on_call", "name": "quit" }, { "id": "q", "type": "quit_game" } ],
      "links": [ ["go.then", "q.in"] ] })"));
    ticks(2);
    CHECK(g_ui.scr.count(4) == 0);
    CHECK(run("graphs.call('quit', dnp.players()[1], nil)", "the call"));
    ticks(2);
    std::printf("  quit field: %d\n", g_ui.scr.count(4) ? g_ui.scr[4] : -1);
    CHECK(g_ui.scr.count(4) == 1 && g_ui.scr[4] == 1000);
    CHECK(!load_graph(R"({ "graph": 1, "name": "menu_quit2",
      "nodes": [ { "id": "go", "type": "on_call", "name": "q2" }, { "id": "q", "type": "quit_game", "now": true } ],
      "links": [ ["go.then", "q.in"] ] })"));     // an unknown field: refused, as any is

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
        // A CHAIN OUTLIVES ITS EVENT'S PROP: removing the prop the event names
        // (despawn, who: target) and carrying on into a light loop with
        // delays -- the prop gone by the time the delays fire -- keeps looping.
        g_ui.sectors.clear();
        CHECK(load_graph(R"({ "graph": 1, "name": "relicprobe",
          "nodes": [ { "id": "u", "type": "when_prop_touched", "prop": "relic" },
                     { "id": "d", "type": "action", "a": "despawn", "who": "target" },
                     { "id": "l2", "type": "set_sector_light", "tag": 1, "light": 20 },
                     { "id": "w", "type": "delay", "ticks": 8 },
                     { "id": "l3", "type": "set_sector_light", "tag": 1, "light": 110 },
                     { "id": "w2", "type": "delay", "ticks": 15 } ],
          "links": [ ["u.then", "d.in"], ["d.then", "l2.in"], ["l2.then", "w.in"], ["w.then", "l3.in"], ["l3.then", "w2.in"], ["w2.then", "l2.in"] ] })"));
        CHECK(run("local alive = true\n"
                  "relic = { index = function() return 777 end, name = function() return 'relic' end,\n"
                  "  valid = function() return alive end, despawn = function() alive = false; return true end }\n"
                  "probe_ok = pcall(graphs.touched, dnp.players()[1], relic)\n"
                  "probe_gone = relic:valid() and 0 or 1", "touch the relic"));
        ticks(60);
        std::printf("  a light loop after its prop was removed: %zu changes in 60 ticks\n", g_ui.sectors.size());
        CHECK(truthy("probe_ok") && num("probe_gone") == 1);   // CONTROL: the prop really went
        CHECK(g_ui.sectors.size() >= 4);                       // 20, 110, 20, 110...
        // CONTROL: a tag no sector carries changes nothing.
        g_ui.sectors.clear();
        CHECK(load_graph(R"({ "graph": 1, "name": "nowhere",
          "nodes": [ { "id": "go", "type": "on_call", "name": "nowhere" },
                     { "id": "l", "type": "set_sector_light", "tag": 77, "light": 0 } ],
          "links": [ ["go.then", "l.in"] ] })"));
        CHECK(run("graphs.call('nowhere', nil, nil)", "nowhere") && g_ui.sectors.empty());
        // A trigger verb addressed BY TAG from a graph, in a level with no
        // trigger table: the tag index is built on first use and must hold
        // sectors, as the loader's does -- it held their numbers, and the move
        // died indexing one ("attempt to index local 's'").
        CHECK(load_graph(R"({ "graph": 1, "name": "bytag",
          "nodes": [ { "id": "go", "type": "on_call", "name": "raise" },
                     { "id": "m", "type": "action", "a": "sector_move", "tag": 3, "surface": "ceiling",
                       "to": "absolute", "height": "12", "speed": 1, "loop": "once" } ],
          "links": [ ["go.then", "m.in"] ] })"));
        CHECK(run("bt_ok, bt_err = pcall(graphs.call, 'raise', nil, nil); bt_err = tostring(bt_err)", "a move by tag"));
        if (!truthy("bt_ok")) { lua_getglobal(L, "bt_err"); std::printf("  %s\n", lua_tostring(L, -1)); lua_pop(L, 1); }
        CHECK(truthy("bt_ok"));
        ticks(60);
        CHECK(run("bt_ceil = dnp.sector(2):ceil().raw / 4294967296", "the ceiling after"));
        std::printf("  sector_move by tag 3: sector 2's ceiling now %.3f\n", num("bt_ceil"));
        CHECK(num("bt_ceil") == 12);
        // A push STRAIGHT UP (a spring): the strength goes into the vertical.
        // It went nowhere -- `up` was a direction with no case, so only the
        // separate `up` lift moved anything.
        CHECK(load_graph(R"({ "graph": 1, "name": "spring",
          "nodes": [ { "id": "go", "type": "on_call", "name": "boing" },
                     { "id": "p", "type": "push", "direction": "up", "strength": 12 } ],
          "links": [ ["go.then", "p.in"] ] })"));
        CHECK(run("graphs.call('boing', dnp.players()[1], nil)", "a spring"));
        ticks(1);
        CHECK(run("sp_vz = dnp.players()[1]:vel().z.raw / 4294967296", "the push up"));
        std::printf("  push up 12: vertical speed %.2f\n", num("sp_vz"));
        CHECK(num("sp_vz") > 5);
    }

    std::printf("-- arrivals, after the level stood empty --\n");
    {
        CHECK(load_graph(R"({ "graph": 1, "name": "roster", "vars": { "came": 0, "went": 0 },
          "nodes": [ { "id": "a", "type": "when_player_arrives" }, { "id": "c", "type": "add_var", "var": "came", "value": 1 },
                     { "id": "l", "type": "when_player_leaves" },  { "id": "w", "type": "add_var", "var": "went", "value": 1 } ],
          "links": [ ["a.then", "c.in"], ["l.then", "w.in"] ] })"));
        ticks(5);
        CHECK(run("r_came = graphs.var('roster', 'came'); r_went = graphs.var('roster', 'went')", "first arrival"));
        CHECK(num("r_came") == 1 && num("r_went") == 0);      // once, and not again while it stays
        // The level empties and stops ticking (its clock keeps count); the
        // player is back later in the same slot. That is a new arrival.
        for (int i = 0; i < 120; ++i) g_world->step();
        ticks(3);
        CHECK(run("r_came = graphs.var('roster', 'came'); r_went = graphs.var('roster', 'went')", "back again"));
        std::printf("  arrivals %g, departures %g\n", num("r_came"), num("r_went"));
        CHECK(num("r_came") == 2 && num("r_went") == 1);
    }

    std::printf("-- panels by name (docs/PANELS.md) --\n");
    {
        CHECK(load_graph(R"({ "graph": 1, "name": "shopg",
          "vars": { "pressed": 0, "last": "", "closed": 0, "open_now": false },
          "panels": { "shop": { "title": "Shop", "template": "shop", "widgets": [
              { "kind": "label",  "id": 1, "name": "gold",  "label": "Gold" },
              { "kind": "button", "id": 2, "name": "buy",   "label": "Buy" },
              { "kind": "slider", "id": 3, "name": "count", "label": "How many", "lo": 1, "hi": 5, "value": 1 } ] } },
          "nodes": [ { "id": "o",  "type": "on_call", "name": "open" },  { "id": "op", "type": "open_panel", "panel": "shop" },
                     { "id": "c",  "type": "on_call", "name": "close" }, { "id": "cp", "type": "close_panel", "panel": "shop" },
                     { "id": "s",  "type": "on_call", "name": "set" },   { "id": "sw", "type": "set_widget", "panel": "shop", "widget": "count", "value": 4 },
                     { "id": "ub", "type": "when_panel_used", "panel": "shop", "widget": "buy" },
                     { "id": "ap", "type": "add_var", "var": "pressed", "value": 1 },
                     { "id": "ua", "type": "when_panel_used", "panel": "shop" },
                     { "id": "sl", "type": "set_var", "var": "last" },
                     { "id": "cl", "type": "when_panel_closed", "panel": "shop" },
                     { "id": "ac", "type": "add_var", "var": "closed", "value": 1 },
                     { "id": "k",  "type": "on_call", "name": "check" },
                     { "id": "io", "type": "panel_is_open", "panel": "shop" },
                     { "id": "so", "type": "set_var", "var": "open_now" } ],
          "links": [ ["o.then", "op.in"], ["c.then", "cp.in"], ["s.then", "sw.in"], ["ub.then", "ap.in"],
                     ["ua.then", "sl.in"], ["ua.widget", "sl.value"], ["cl.then", "ac.in"],
                     ["k.then", "so.in"], ["io.value", "so.value"] ] })"));
        auto var = [&](const char* v) {
            const std::string src = std::string("pv = graphs.var('shopg', '") + v + "')";
            run(src.c_str(), v);
            lua_getglobal(L, "pv");
            std::string out = lua_isboolean(L, -1) ? (lua_toboolean(L, -1) ? "true" : "false")
                                                   : (lua_tostring(L, -1) ? lua_tostring(L, -1) : "nil");
            lua_pop(L, 1);
            return out;
        };
        g_ui.opened.clear(); g_ui.closed_ids.clear(); g_ui.sets.clear();
        CHECK(run("graphs.call('open', dnp.players()[1], nil)", "open"));
        CHECK(g_ui.opened.size() == 1 && g_ui.opened[0].count == 3 && g_ui.opened[0].template_len == 4 &&
              g_ui.opened[0].title.len == 4);
        const u32 first = g_ui.next_ui;
        CHECK(run("graphs.call('open', dnp.players()[1], nil)", "open again"));
        CHECK(g_ui.opened.size() == 2 && g_ui.closed_ids.size() == 1 && g_ui.closed_ids[0] == first);
        const u32 id = g_ui.next_ui;
        // A press by number arrives; the graph hears it by name.
        const std::string press = "graphs.ui_event(dnp.players()[1], " + std::to_string(id) + ", 2, 1)";
        CHECK(run(press.c_str(), "press buy"));
        CHECK(var("pressed") == "1" && var("last") == "buy");
        const std::string slide = "graphs.ui_event(dnp.players()[1], " + std::to_string(id) + ", 3, 4)";
        CHECK(run(slide.c_str(), "slide"));
        CHECK(var("pressed") == "1" && var("last") == "count");   // `widget` given: only its own
        // CONTROL: a panel this VM's graphs did not open (a trigger table's) fires nothing.
        CHECK(run("graphs.ui_event(dnp.players()[1], 9999, 2, 1)", "a trigger table's press"));
        CHECK(var("pressed") == "1");
        CHECK(run("graphs.call('set', dnp.players()[1], nil)", "set"));
        CHECK(g_ui.sets.size() == 1 && g_ui.sets[0].ui == id && g_ui.sets[0].widget == 3 && g_ui.sets[0].value == 4);
        CHECK(run("graphs.call('check', dnp.players()[1], nil)", "check") && var("open_now") == "true");
        // The ×: widget 0. Fires when_panel_closed, and the panel is forgotten.
        const std::string x = "graphs.ui_event(dnp.players()[1], " + std::to_string(id) + ", 0, 0)";
        CHECK(run(x.c_str(), "the x"));
        CHECK(var("closed") == "1");
        CHECK(run("graphs.call('check', dnp.players()[1], nil)", "check") && var("open_now") == "false");
        CHECK(run("graphs.call('set', dnp.players()[1], nil)", "set when closed") && g_ui.sets.size() == 1);
        CHECK(run("graphs.call('close', dnp.players()[1], nil)", "close when closed") && g_ui.closed_ids.size() == 1);
        CHECK(run(press.c_str(), "a press after the x") && var("pressed") == "1");   // gone, and quiet
        // close_panel on an open one closes it, and fires no when_panel_closed.
        CHECK(run("graphs.call('open', dnp.players()[1], nil)", "open"));
        CHECK(run("graphs.call('close', dnp.players()[1], nil)", "close"));
        CHECK(g_ui.closed_ids.size() == 2 && g_ui.closed_ids[1] == g_ui.next_ui && var("closed") == "1");

        // LOAD ERRORS, each refused.
        const char* bad[] = {
          R"({ "graph": 1, "name": "b1", "nodes": [ { "id": "o", "type": "open_panel", "panel": "nope" } ], "links": [] })",
          R"({ "graph": 1, "name": "b2", "panels": { "p": { "widgets": [ { "kind": "button", "id": 1, "name": "a", "label": "A" } ] } },
               "nodes": [ { "id": "u", "type": "when_panel_used", "panel": "p", "widget": "zz" } ], "links": [] })",
          R"({ "graph": 1, "name": "b3", "panels": { "p": { "widgets": [ { "kind": "button", "id": 1, "name": "a", "label": "A" },
               { "kind": "button", "id": 2, "name": "a", "label": "B" } ] } }, "nodes": [], "links": [] })",
          R"({ "graph": 1, "name": "b4", "panels": { "p": { "widgets": [ { "kind": "dial", "id": 1, "name": "a", "label": "A" } ] } },
               "nodes": [], "links": [] })",
          R"({ "graph": 1, "name": "b5", "panels": { "p": { "widgets": [ { "kind": "button", "id": 1, "name": "a", "label": "A" } ] } },
               "nodes": [ { "id": "s", "type": "set_widget", "panel": "p", "widget": "a", "value": 1 } ], "links": [] })",
          R"({ "graph": 1, "name": "b6", "panels": { "Bad Name": { "widgets": [] } }, "nodes": [], "links": [] })",
          R"({ "graph": 1, "name": "b7", "panels": { "p": { "widgets": [ { "kind": "button", "id": 0, "name": "a", "label": "A" } ] } },
               "nodes": [], "links": [] })" };
        for (const char* b : bad) CHECK(!load_graph(b));
        {
            std::string many = R"({ "graph": 1, "name": "b8", "panels": { "p": { "widgets": [ )";
            for (int i = 1; i <= 33; ++i)
                many += (i > 1 ? "," : "") + std::string(R"({ "kind": "button", "id": )") + std::to_string(i) +
                        R"(, "name": "w)" + std::to_string(i) + R"(", "label": "W" })";
            many += " ] } }, \"nodes\": [], \"links\": [] }";
            CHECK(!load_graph(many.c_str()));
        }
        // set_hud: to the player, and to everyone.
        g_ui.huds.clear();
        CHECK(load_graph(R"({ "graph": 1, "name": "hudg",
          "nodes": [ { "id": "c", "type": "on_call", "name": "gold" },
                     { "id": "h", "type": "set_hud", "name": "gold", "value": 40 } ],
          "links": [ ["c.then", "h.in"] ] })"));
        CHECK(run("graphs.call('gold', dnp.players()[1], nil)", "set_hud"));
        CHECK(g_ui.huds.count("gold") == 1);
    }

    std::printf("-- markers: dnp.mark and the graph nodes (docs/MARKERS.md) --\n");
    {
        // THE BINDING. On a place: a point, in the world's frame; values merge.
        g_ui.marks.clear();
        CHECK(run("sign = dnp.mark(vec3.new(fx.int(3), fx.int(4), fx.int(1)), 'label', { text = 'Mill' })", "dnp.mark"));
        const u32 sign = static_cast<u32>(num("sign"));
        CHECK(g_ui.marks.count(sign) == 1 && g_ui.marks[sign].m.anchor == proto::MARK_AT_POINT &&
              g_ui.marks[sign].m.pos.x == F(3) && g_ui.marks[sign].m.pos.y == F(4) &&
              g_ui.value(sign, "text") == "Mill");
        CHECK(run("set_ok = dnp.mark_set(sign, { text = false, value = 3 })", "dnp.mark_set"));
        CHECK(truthy("set_ok") && g_ui.value(sign, "text") == "<none>" && g_ui.value(sign, "value") == "3");
        CHECK(run("dnp.unmark(sign); gone_ok = dnp.mark_set(sign, { value = 4 })", "dnp.unmark"));
        CHECK(g_ui.marks.count(sign) == 0 && !truthy("gone_ok"));
        // On a body: its handle as the snapshots carry it, index + 1 and its generation.
        CHECK(run("on_me = dnp.mark(dnp.players()[1], 'health', { value = 7, max = 9 }, "
                  "{ height = fx.frac(1, 2), range = fx.int(20), through_walls = true })", "dnp.mark on a body"));
        const u32 on_me = static_cast<u32>(num("on_me"));
        CHECK(g_ui.marks.count(on_me) == 1 && g_ui.marks[on_me].m.anchor == proto::MARK_ON_ENTITY &&
              (g_ui.marks[on_me].m.handle.raw & 0x00FFFFFFu) == player + 1 &&
              (g_ui.marks[on_me].m.handle.raw >> 24) == world.generation(player) &&
              g_ui.marks[on_me].m.height == Ff(1, 2) && g_ui.marks[on_me].m.range == F(20) &&
              (g_ui.marks[on_me].m.flags & proto::MARK_FLAG_THROUGH) != 0);
        // A DEBUG marker is not even sent when the module ships without the overlay.
        ctx.debug_overlay = false;
        CHECK(run("dbg_off = dnp.mark(dnp.players()[1], 'debug_health', { value = 1 }, { debug = true })", "debug off"));
        ctx.debug_overlay = true;
        CHECK(run("dbg_on = dnp.mark(dnp.players()[1], 'debug_health', { value = 1 }, { debug = true })", "debug on"));
        CHECK(!truthy("dbg_off") && truthy("dbg_on"));
        CHECK(run("dbg_flag = dnp.debug_overlay()", "dnp.debug_overlay") && truthy("dbg_flag"));
        // CONTROLS: a template that is not a name, and a value that is an fx.
        CHECK(raises("dnp.mark(dnp.players()[1], 'Big Bar', {})"));
        CHECK(raises("dnp.mark(dnp.players()[1], 'health', { value = fx.frac(1, 2) })"));
        CHECK(!raises("dnp.unmark(dnp.mark(dnp.players()[1], 'health', { value = 1 }))"));   // CONTROL

        // THE GRAPH NODES. A bar hung on the caller, hung again (one, not two),
        // changed, then taken down; and a sign at a place only the caller sees.
        g_ui.marks.clear();
        g_ui.unmarked = 0;
        CHECK(load_graph(R"({ "graph": 1, "name": "markg",
          "nodes": [ { "id": "t", "type": "on_call", "name": "tag" },
                     { "id": "m", "type": "mark", "marker": "bar", "template": "health", "value": 40, "max": 100, "height": 0.25 },
                     { "id": "h", "type": "on_call", "name": "hit" },
                     { "id": "s", "type": "set_marker", "marker": "bar", "value": 25 },
                     { "id": "d", "type": "on_call", "name": "drop" },
                     { "id": "r", "type": "remove_marker", "marker": "bar" },
                     { "id": "p", "type": "on_call", "name": "sign" },
                     { "id": "q", "type": "mark", "marker": "sign", "template": "label", "at": "place",
                       "x": 2, "y": -3, "z": 0, "text": "Exit", "to": "player" } ],
          "links": [ ["t.then", "m.in"], ["h.then", "s.in"], ["d.then", "r.in"], ["p.then", "q.in"] ] })"));
        CHECK(run("graphs.call('tag', dnp.players()[1], nil)", "mark"));
        CHECK(g_ui.marks.size() == 1);
        const u32 bar = g_ui.marks.begin()->first;
        CHECK(g_ui.value(bar, "value") == "40" && g_ui.value(bar, "max") == "100" &&
              (g_ui.marks[bar].m.handle.raw & 0x00FFFFFFu) == player + 1 &&
              std::string(g_ui.marks[bar].m.template_name, g_ui.marks[bar].m.template_len) == "health" &&
              g_ui.marks[bar].m.height == Ff(1, 4));
        CHECK(run("graphs.call('tag', dnp.players()[1], nil)", "mark again"));
        CHECK(g_ui.marks.size() == 1 && g_ui.unmarked == 1);          // replaced, not doubled
        const u32 bar2 = g_ui.marks.begin()->first;
        CHECK(run("graphs.call('hit', dnp.players()[1], nil)", "set_marker"));
        CHECK(g_ui.value(bar2, "value") == "25" && g_ui.value(bar2, "max") == "100");
        CHECK(run("graphs.call('sign', dnp.players()[1], nil)", "a place marker"));
        u32 sign_id = 0;
        for (const auto& kv : g_ui.marks) if (kv.second.m.anchor == proto::MARK_AT_POINT) sign_id = kv.first;
        CHECK(sign_id != 0 && g_ui.marks[sign_id].to == static_cast<i32>(player) &&
              g_ui.marks[sign_id].m.pos.x == F(2) && g_ui.marks[sign_id].m.pos.y == -F(3) &&
              g_ui.value(sign_id, "text") == "Exit");
        CHECK(run("graphs.call('drop', dnp.players()[1], nil)", "remove_marker"));
        CHECK(g_ui.marks.count(bar2) == 0 && g_ui.marks.count(sign_id) == 1);   // the bar, not the sign
        // CONTROL: a template that is not a name is refused at load, by node.
        CHECK(!load_graph(R"({ "graph": 1, "name": "markbad",
          "nodes": [ { "id": "t", "type": "on_call", "name": "x" },
                     { "id": "m", "type": "mark", "marker": "bar", "template": "Big Bar" } ],
          "links": [ ["t.then", "m.in"] ] })"));
    }

    std::printf("-- entity properties through get_var / set_var / add_var --\n");
    {
        // The movement list is the engine's parameter table, both ways.
        int listed = 0, missing = 0;
        for (u16 i = 0; i < physics::PARAM_COUNT; ++i) {
            const std::string src = std::string("pp = false; for _, p in ipairs(graph_vocab.properties) do "
                                                "if p.name == '") + physics::PARAM_TABLE[i].name +
                                    "' and p.of == 'movement' then pp = true end end";
            run(src.c_str(), "parity");
            if (truthy("pp")) ++listed; else { ++missing; std::printf("  not listed: %s\n", physics::PARAM_TABLE[i].name); }
        }
        CHECK(missing == 0 && listed == physics::PARAM_COUNT && listed > 40);
        CHECK(run("extra = 0; for _, p in ipairs(graph_vocab.properties) do if p.of == 'movement' then "
                  "local ok = pcall(function() return dnp.players()[1]:param(p.name) end); if not ok then extra = extra + 1 end end end",
                  "every listed movement property is the engine's"));
        CHECK(num("extra") == 0);

        CHECK(load_graph(R"({ "graph": 1, "name": "props", "vars": { "seen_x": 0, "seen_hp": 0 },
          "nodes": [ { "id": "r", "type": "on_call", "name": "read" },
                     { "id": "gx", "type": "get_var", "var": "x" }, { "id": "sx", "type": "set_var", "var": "seen_x" },
                     { "id": "gh", "type": "get_var", "var": "hp" }, { "id": "sh", "type": "set_var", "var": "seen_hp" },
                     { "id": "w", "type": "on_call", "name": "write" },
                     { "id": "hp", "type": "set_var", "var": "hp", "value": 40 },
                     { "id": "heal", "type": "add_var", "var": "hp", "value": 15 },
                     { "id": "ms", "type": "set_var", "var": "move_speed", "value": 3.5 },
                     { "id": "fast", "type": "on_call", "name": "fast" },
                     { "id": "ms2", "type": "set_var", "var": "move_speed", "value": 12 } ],
          "links": [ ["r.then", "sx.in"], ["gx.value", "sx.value"], ["sx.then", "sh.in"], ["gh.value", "sh.value"],
                     ["w.then", "hp.in"], ["hp.then", "heal.in"], ["heal.then", "ms.in"], ["fast.then", "ms2.in"] ] })"));
        CHECK(run("dnp.players()[1]:set_hp(100); graphs.call('read', dnp.players()[1], nil)", "read"));
        CHECK(run("px = dnp.players()[1]:pos().x.raw / 4294967296", "x") );
        CHECK(run("sx = graphs.var('props', 'seen_x'); sh = graphs.var('props', 'seen_hp')", "seen"));
        std::printf("  read: x %.2f (body %.2f), hp %g\n", num("sx"), num("px"), num("sh"));
        CHECK(num("sx") == num("px") && num("sh") == 100);
        CHECK(run("graphs.call('write', dnp.players()[1], nil)", "write"));
        CHECK(run("hpnow = dnp.players()[1]:hp()", "hp") && num("hpnow") == 55);   // set 40, then +15
        const physics::MoveParams slow = effects.compute(player);
        CHECK(slow.move_speed == fx::from_frac(7, 2));
        // A second set REPLACES the first, rather than stacking another effect.
        CHECK(run("graphs.call('fast', dnp.players()[1], nil)", "fast"));
        const physics::MoveParams fast = effects.compute(player);
        std::printf("  move_speed set 3.5 then 12: %g, %g\n", slow.move_speed.raw / 4294967296.0, fast.move_speed.raw / 4294967296.0);
        CHECK(fast.move_speed == F(12));

        // REFUSED AT LOAD: a variable taking a property's name, setting what
        // can only be read, adding to what is not a number, a name that is neither.
        const char* bad[] = {
          R"({ "graph": 1, "name": "v1", "vars": { "hp": 1 }, "nodes": [], "links": [] })",
          R"({ "graph": 1, "name": "v2", "nodes": [ { "id": "s", "type": "set_var", "var": "x", "value": 1 } ], "links": [] })",
          R"({ "graph": 1, "name": "v3", "nodes": [ { "id": "s", "type": "add_var", "var": "climb_enabled", "value": 1 } ], "links": [] })",
          R"({ "graph": 1, "name": "v4", "nodes": [ { "id": "s", "type": "get_var", "var": "luck" } ], "links": [] })" };
        for (const char* b : bad) CHECK(!load_graph(b));
        // CONTROL: a graph's own variable is still a variable.
        CHECK(load_graph(R"({ "graph": 1, "name": "v5", "vars": { "luck": 3 },
          "nodes": [ { "id": "g", "type": "get_var", "var": "luck" } ], "links": [] })"));
    }

    std::printf("-- numbers from links (docs/GRAPH_NUMBERS.md) --\n");
    {
        // An action: the light from a variable; a required field fed by a link
        // needs no literal. CONTROL: the literal when unlinked.
        g_ui.sectors.clear();
        CHECK(load_graph(R"({ "graph": 1, "name": "nums", "vars": { "dim": 25, "wait": 3, "n": 0, "every_s": 0.05, "hits": 0 },
          "nodes": [ { "id": "go", "type": "on_call", "name": "dim" },
                     { "id": "v", "type": "get_var", "var": "dim" },
                     { "id": "l", "type": "set_sector_light", "sector": 0 },
                     { "id": "lit", "type": "set_sector_light", "sector": 1, "light": 200 },
                     { "id": "lw", "type": "on_call", "name": "later" },
                     { "id": "w", "type": "get_var", "var": "wait" },
                     { "id": "d", "type": "delay" },
                     { "id": "inc", "type": "add_var", "var": "n", "value": 1 },
                     { "id": "ev", "type": "every" },
                     { "id": "es", "type": "get_var", "var": "every_s" },
                     { "id": "hit", "type": "add_var", "var": "hits", "value": 1 } ],
          "links": [ ["go.then", "l.in"], ["v.value", "l.light"], ["l.then", "lit.in"],
                     ["lw.then", "d.in"], ["w.value", "d.ticks"], ["d.then", "inc.in"],
                     ["es.value", "ev.seconds"], ["ev.then", "hit.in"] ] })"));
        CHECK(run("graphs.call('dim', nil, nil)", "dim"));
        CHECK(g_ui.sectors.size() == 2 && g_ui.sectors[0].light == 25 && g_ui.sectors[1].light == 200);
        // Flow: the delay's ticks from a variable -- 3 ticks, not the default.
        CHECK(run("graphs.call('later', nil, nil)", "later"));
        ticks(2);
        const double early = [&] { run("nv = graphs.var('nums', 'n')", "n"); return num("nv"); }();
        ticks(3);
        run("nv = graphs.var('nums', 'n')", "n");
        std::printf("  delay from a variable: after 2 ticks %g, after 5 %g\n", early, num("nv"));
        CHECK(early == 0 && num("nv") == 1);
        // A start: every 0.05 s (3 ticks), from a variable, over 30 ticks.
        run("h0 = graphs.var('nums', 'hits')", "hits");
        const double before = num("h0");
        ticks(30);
        run("h1 = graphs.var('nums', 'hits')", "hits");
        std::printf("  every from a variable: %g fires in 30 ticks\n", num("h1") - before);
        CHECK(num("h1") - before >= 9 && num("h1") - before <= 11);
        // Data: random's bounds linked.
        CHECK(load_graph(R"({ "graph": 1, "name": "rnd", "vars": { "seven": 7, "got": 0 },
          "nodes": [ { "id": "go", "type": "on_call", "name": "roll" },
                     { "id": "s", "type": "get_var", "var": "seven" },
                     { "id": "r", "type": "random" },
                     { "id": "keep", "type": "set_var", "var": "got" } ],
          "links": [ ["go.then", "keep.in"], ["s.value", "r.lo"], ["s.value", "r.hi"], ["r.value", "keep.value"] ] })"));
        CHECK(run("graphs.call('roll', nil, nil); rg = graphs.var('rnd', 'got')", "roll") && num("rg") == 7);
        // CONTROL: a required number neither given nor linked is still refused.
        CHECK(!load_graph(R"({ "graph": 1, "name": "nolight",
          "nodes": [ { "id": "l", "type": "set_sector_light", "sector": 0 } ], "links": [] })"));
        // A VERB: set_hp's value from a variable satisfies "one of value, add".
        CHECK(load_graph(R"({ "graph": 1, "name": "hpv", "vars": { "to": 33 },
          "nodes": [ { "id": "go", "type": "on_call", "name": "hp" },
                     { "id": "v", "type": "get_var", "var": "to" },
                     { "id": "a", "type": "action", "a": "set_hp" } ],
          "links": [ ["go.then", "a.in"], ["v.value", "a.value"] ] })"));
        CHECK(run("graphs.call('hp', dnp.players()[1], nil); hpv = dnp.players()[1]:hp()", "set_hp") && num("hpv") == 33);
        // CONTROL: with neither given nor linked, it is still refused.
        CHECK(!load_graph(R"({ "graph": 1, "name": "hpnone",
          "nodes": [ { "id": "go", "type": "on_call", "name": "hp2" }, { "id": "a", "type": "action", "a": "set_hp" } ],
          "links": [ ["go.then", "a.in"] ] })"));
    }

    std::printf("-- a camera turned by a timer (every -> add_var -> camera_follow) --\n");
    {
        // The user's graph: an `every` adds to a variable, and the camera's
        // angle is that variable. A timer has no player in its event; the
        // camera node must still reach the players.
        g_ui.cams.clear();
        CHECK(load_graph(R"({ "graph": 1, "name": "spin", "vars": { "camangle": 0 },
          "nodes": [ { "id": "t", "type": "every", "seconds": 0.1 },
                     { "id": "add", "type": "add_var", "var": "camangle", "value": 10 },
                     { "id": "a", "type": "get_var", "var": "camangle" },
                     { "id": "cam", "type": "camera_follow" } ],
          "links": [ ["t.then", "add.in"], ["add.then", "cam.in"], ["a.value", "cam.angle"] ] })"));
        ticks(13);
        const bool sent = g_ui.cams.count(8) == 1;   // field 8: yaw, in thousandths of a turn
        std::printf("  yaw sent: %s (%d thousandths)\n", sent ? "yes" : "no", sent ? g_ui.cams[8] : -1);
        CHECK(sent && g_ui.cams[8] > 0);
        const i32 first = sent ? g_ui.cams[8] : 0;
        ticks(12);
        CHECK(g_ui.cams.count(8) == 1 && g_ui.cams[8] > first);   // and it keeps turning
    }

    std::printf("-- buttons and the module's own keys (docs/KEYS.md) --\n");
    {
        static std::vector<std::string> keys = { "reload", "inventory" };
        ctx.actions = &keys;
        CHECK(load_graph(R"({ "graph": 1, "name": "keys", "vars": { "jumps": 0, "lets": 0, "reloads": 0, "inv": false },
          "nodes": [ { "id": "jp", "type": "when_button_pressed", "button": "jump" }, { "id": "a1", "type": "add_var", "var": "jumps", "value": 1 },
                     { "id": "jr", "type": "when_button_released", "button": "jump" }, { "id": "a2", "type": "add_var", "var": "lets", "value": 1 },
                     { "id": "rp", "type": "when_button_pressed", "button": "reload" }, { "id": "a3", "type": "add_var", "var": "reloads", "value": 1 },
                     { "id": "k", "type": "on_call", "name": "inv" },
                     { "id": "bh", "type": "button_held", "button": "inventory" }, { "id": "si", "type": "set_var", "var": "inv" } ],
          "links": [ ["jp.then", "a1.in"], ["jr.then", "a2.in"], ["rp.then", "a3.in"], ["k.then", "si.in"], ["bh.value", "si.value"] ] })"));
        auto press = [&](u16 buttons, u8 actions, int n) {
            proto::InputFrame f;
            f.buttons = buttons;
            f.actions = actions;
            world.set_input(player, f);
            ticks(n);
        };
        auto v = [&](const char* name) {
            const std::string src = std::string("kv = graphs.var('keys', '") + name + "')";
            run(src.c_str(), name);
            lua_getglobal(L, "kv");
            const double out = lua_isboolean(L, -1) ? (lua_toboolean(L, -1) ? 1 : 0) : lua_tonumber(L, -1);
            lua_pop(L, 1);
            return out;
        };
        press(0, 0, 2);
        press(0x0001, 0, 5);                          // jump, held five ticks: one press
        CHECK(v("jumps") == 1 && v("lets") == 0);
        press(0, 0, 2);                               // let go: one release
        CHECK(v("jumps") == 1 && v("lets") == 1);
        press(0, 0x01, 3);                            // the module's `reload` (action 0)
        press(0, 0x00, 1);
        press(0, 0x01, 1);                            // and again
        std::printf("  jump pressed %g, released %g; reload pressed %g\n", v("jumps"), v("lets"), v("reloads"));
        CHECK(v("reloads") == 2);
        press(0, 0x02, 1);                            // `inventory` (action 1) held
        CHECK(run("graphs.call('inv', dnp.players()[1], nil)", "held") && v("inv") == 1);
        press(0, 0x01, 1);                            // CONTROL: another key is not it
        CHECK(run("graphs.call('inv', dnp.players()[1], nil)", "not held") && v("inv") == 0);
        press(0, 0, 1);
        // A button nobody has is refused at load, naming what there is.
        CHECK(!load_graph(R"({ "graph": 1, "name": "nokey",
          "nodes": [ { "id": "x", "type": "when_button_pressed", "button": "teleport" } ], "links": [] })"));
        ctx.actions = nullptr;
    }
    std::printf("-- a pinned panel --\n");
    {
        g_ui.opened.clear();
        CHECK(load_graph(R"({ "graph": 1, "name": "hud_panel",
          "panels": { "arms": { "title": "Arms", "pinned": true, "widgets": [ { "kind": "label", "id": 1, "name": "gun", "label": "Gun" } ] } },
          "nodes": [ { "id": "c", "type": "on_call", "name": "show" }, { "id": "o", "type": "open_panel", "panel": "arms" } ],
          "links": [ ["c.then", "o.in"] ] })"));
        CHECK(run("graphs.call('show', dnp.players()[1], nil)", "show"));
        CHECK(g_ui.opened.size() == 1 && (g_ui.opened[0].flags & proto::UI_FLAG_PINNED) != 0);
    }

    // ================================================================
    // A TYPED SUBTITLE: the line, and how the client is to type it out.
    std::printf("-- subtitle, typed --\n");
    {
        g_ui.huds.clear();
        CHECK(load_graph(R"({ "graph": 1, "name": "typed",
          "nodes": [ { "id": "c", "type": "on_call", "name": "say_typed" },
                     { "id": "s", "type": "subtitle", "everyone": true, "text": "Gems ahead.", "mode": "typed",
                       "speed": 20, "pack": 3, "sound": "ui.type" },
                     { "id": "c2", "type": "on_call", "name": "say_line" },
                     { "id": "l", "type": "subtitle", "everyone": true, "text": "Plain." } ],
          "links": [ ["c.then", "s.in"], ["c2.then", "l.in"] ] })"));
        CHECK(run("graphs.call('say_typed', dnp.players()[1], nil)", "typed"));
        std::printf("  subtitle '%s', typing %s, pack %s, sound %s\n", g_ui.huds["subtitle"].c_str(),
                    g_ui.huds["subtitle_typing"].c_str(), g_ui.huds["subtitle_pack"].c_str(), g_ui.huds["subtitle_sound"].c_str());
        CHECK(g_ui.huds["subtitle"] == "Gems ahead.");
        CHECK(g_ui.huds["subtitle_typing"] == "20" && g_ui.huds["subtitle_pack"] == "3" && g_ui.huds["subtitle_sound"] == "ui.type");
        // CONTROL: a plain line clears how the last one was typed, or it would be typed too.
        CHECK(run("graphs.call('say_line', dnp.players()[1], nil)", "line"));
        CHECK(g_ui.huds["subtitle"] == "Plain." && g_ui.huds["subtitle_typing"] == "<cleared>" &&
              g_ui.huds["subtitle_sound"] == "<cleared>");
        // A typed mode with an unknown name is refused at load.
        CHECK(!load_graph(R"({ "graph": 1, "name": "badmode",
          "nodes": [ { "id": "s", "type": "subtitle", "text": "x", "mode": "scrolling" } ], "links": [] })"));
    }

    // ================================================================
    // THE CAMERA'S LEAD, from camera_follow: `lead` (field 4) and the lead
    // that grows with forward speed, `lead_velocity` (field 18).
    std::printf("-- camera_follow: a lead, and one that grows with speed --\n");
    {
        g_ui.cams.clear();
        CHECK(load_graph(R"({ "graph": 1, "name": "leadcam",
          "nodes": [ { "id": "c", "type": "on_call", "name": "lead_on" },
                     { "id": "f", "type": "camera_follow", "everyone": true, "lead": 2, "lead_velocity": 0.25 } ],
          "links": [ ["c.then", "f.in"] ] })"));
        CHECK(run("graphs.call('lead_on', dnp.players()[1], nil)", "lead on"));
        std::printf("  lead %d, lead_velocity %d (thousandths)\n",
                    g_ui.cams.count(4) ? g_ui.cams[4] : -1, g_ui.cams.count(18) ? g_ui.cams[18] : -1);
        CHECK(g_ui.cams.count(4) && g_ui.cams[4] == 2000);
        CHECK(g_ui.cams.count(18) && g_ui.cams[18] == 250);
        // CONTROL: a camera_follow that says neither leaves both alone.
        g_ui.cams.clear();
        CHECK(load_graph(R"({ "graph": 1, "name": "plaincam",
          "nodes": [ { "id": "c", "type": "on_call", "name": "plain_on" }, { "id": "f", "type": "camera_follow", "everyone": true } ],
          "links": [ ["c.then", "f.in"] ] })"));
        CHECK(run("graphs.call('plain_on', dnp.players()[1], nil)", "plain"));
        CHECK(!g_ui.cams.count(4) && !g_ui.cams.count(18));
    }

    // ================================================================
    // SHOWING AND HIDING part of a panel: show_element / hide_element.
    std::printf("-- panel elements shown and hidden --\n");
    {
        g_ui.opened.clear();
        g_ui.shows.clear();
        CHECK(load_graph(R"({ "graph": 1, "name": "shop_panel",
          "panels": { "stall": { "title": "Stall", "widgets": [
              { "kind": "button", "id": 1, "name": "buy", "label": "Buy" },
              { "kind": "button", "id": 2, "name": "sell", "label": "Sell", "hidden": true },
              { "kind": "label", "id": 3, "name": "note", "label": "Closed" } ] } },
          "nodes": [ { "id": "c0", "type": "on_call", "name": "hide_early" },
                     { "id": "h0", "type": "hide_element", "panel": "stall", "widget": "buy" },
                     { "id": "c1", "type": "on_call", "name": "open_shop" },
                     { "id": "o",  "type": "open_panel", "panel": "stall" },
                     { "id": "c2", "type": "on_call", "name": "rich" },
                     { "id": "s",  "type": "show_element", "panel": "stall", "widget": "sell" },
                     { "id": "h",  "type": "hide_element", "panel": "stall", "widget": "note" },
                     { "id": "d",  "type": "hide_element", "panel": "stall", "decor": "coin_bar" } ],
          "links": [ ["c0.then", "h0.in"], ["c1.then", "o.in"], ["c2.then", "s.in"], ["s.then", "h.in"], ["h.then", "d.in"] ] })"));
        // CONTROL: a panel that is not open has nothing to hide.
        CHECK(run("graphs.call('hide_early', dnp.players()[1], nil)", "hide before open"));
        CHECK(g_ui.shows.empty());
        CHECK(run("graphs.call('open_shop', dnp.players()[1], nil)", "open"));
        const u32 ui = g_ui.next_ui;
        // `hidden = true` starts hidden: the one widget, and only it.
        CHECK(g_ui.shows.size() == 1 && g_ui.shows[0].ui == ui && g_ui.shows[0].widget == 2 && !g_ui.shows[0].shown);
        CHECK(run("graphs.call('rich', dnp.players()[1], nil)", "show and hide"));
        std::printf("  %zu show/hide sent\n", g_ui.shows.size());
        CHECK(g_ui.shows.size() == 4);
        if (g_ui.shows.size() == 4) {
            CHECK(g_ui.shows[1].widget == 2 && g_ui.shows[1].shown && g_ui.shows[1].decor.empty());   // sell shown
            CHECK(g_ui.shows[2].widget == 3 && !g_ui.shows[2].shown);                                 // note hidden
            CHECK(g_ui.shows[3].decor == "coin_bar" && !g_ui.shows[3].shown);                         // decor, by name
        }
        // Refused at load: neither named, both named, a widget the panel lacks, a bad decor id.
        const char* refused[] = {
          R"({ "graph": 1, "name": "x1", "panels": { "p": { "widgets": [ { "kind": "button", "id": 1, "name": "b", "label": "B" } ] } },
               "nodes": [ { "id": "h", "type": "hide_element", "panel": "p" } ], "links": [] })",
          R"({ "graph": 1, "name": "x2", "panels": { "p": { "widgets": [ { "kind": "button", "id": 1, "name": "b", "label": "B" } ] } },
               "nodes": [ { "id": "h", "type": "hide_element", "panel": "p", "widget": "b", "decor": "d" } ], "links": [] })",
          R"({ "graph": 1, "name": "x3", "panels": { "p": { "widgets": [ { "kind": "button", "id": 1, "name": "b", "label": "B" } ] } },
               "nodes": [ { "id": "h", "type": "show_element", "panel": "p", "widget": "nope" } ], "links": [] })",
          R"({ "graph": 1, "name": "x4", "panels": { "p": { "widgets": [ { "kind": "button", "id": 1, "name": "b", "label": "B" } ] } },
               "nodes": [ { "id": "h", "type": "show_element", "panel": "p", "decor": "../x" } ], "links": [] })",
          R"({ "graph": 1, "name": "x5", "panels": { "p": { "widgets": [ { "kind": "button", "id": 1, "name": "b", "label": "B", "hidden": "yes" } ] } },
               "nodes": [], "links": [] })" };
        int n_refused = 0;
        for (const char* j : refused) if (!load_graph(j)) ++n_refused;
        CHECK(n_refused == 5);
    }

    // ================================================================
    // SAVED ON DISK: save_vars / load_vars / forget_saved (docs/GRAPHS.md).
    std::printf("-- saved on disk --\n");
    {
        char dir_tpl[] = "/tmp/dnp-store-XXXXXX";
        const char* dir = mkdtemp(dir_tpl);
        CHECK(dir != nullptr);
        static std::string state_dir;
        state_dir = dir ? dir : "";
        ctx.state_dir = &state_dir;
        const char* keeper = R"({ "graph": 1, "name": "keeper",
          "vars": { "best": 0, "who": "", "beat": false, "saved": 0, "loaded": 0 },
          "nodes": [
            { "id": "c1", "type": "on_call", "name": "win" },
            { "id": "s1", "type": "set_var", "var": "best", "value": 42 },
            { "id": "s2", "type": "set_var", "var": "who", "value": "ana" },
            { "id": "s3", "type": "set_var", "var": "beat", "value": true },
            { "id": "sv", "type": "save_vars", "store": "progress", "vars": "best, who, beat" },
            { "id": "ok", "type": "set_var", "var": "saved", "value": 1 },
            { "id": "no", "type": "set_var", "var": "saved", "value": -1 },
            { "id": "c2", "type": "on_call", "name": "resume" },
            { "id": "lv", "type": "load_vars", "store": "progress" },
            { "id": "l1", "type": "set_var", "var": "loaded", "value": 1 },
            { "id": "l0", "type": "set_var", "var": "loaded", "value": -1 },
            { "id": "c3", "type": "on_call", "name": "reset_progress" },
            { "id": "fg", "type": "forget_saved", "store": "progress" }
          ],
          "links": [ ["c1.then", "s1.in"], ["s1.then", "s2.in"], ["s2.then", "s3.in"], ["s3.then", "sv.in"],
                     ["sv.then", "ok.in"], ["sv.failed", "no.in"],
                     ["c2.then", "lv.in"], ["lv.then", "l1.in"], ["lv.none", "l0.in"],
                     ["c3.then", "fg.in"] ] })";
        auto var = [&](const char* g, const char* k) {
            const std::string src = std::string("t_v = graphs.var('") + g + "', '" + k + "')";
            run(src.c_str(), "reading");
            lua_getglobal(L, "t_v");
            std::string out = lua_isnil(L, -1) ? "nil" : lua_isboolean(L, -1) ? (lua_toboolean(L, -1) ? "true" : "false")
                                                       : std::string(lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
            lua_settop(L, 0);
            return out;
        };
        // Nothing saved yet: `none`.
        CHECK(load_graph(keeper));
        CHECK(run("graphs.call('resume', dnp.players()[1], nil)", "resume, before any save"));
        CHECK(var("keeper", "loaded") == "-1");
        // Saved, and the file is there.
        CHECK(run("graphs.call('win', dnp.players()[1], nil)", "win"));
        CHECK(var("keeper", "saved") == "1");
        const std::string file = state_dir + "/store/progress.store";
        std::FILE* f = std::fopen(file.c_str(), "rb");
        CHECK(f != nullptr);
        if (f) std::fclose(f);
        // A NEW GAME: the graph forgotten and loaded from its file again.
        CHECK(run("graphs.reset()", "reset"));
        CHECK(load_graph(keeper));
        CHECK(var("keeper", "best") == "0");          // CONTROL: a fresh load starts over
        CHECK(run("graphs.call('resume', dnp.players()[1], nil)", "resume"));
        std::printf("  after a new game: best %s, who %s, beat %s, loaded %s\n", var("keeper", "best").c_str(),
                    var("keeper", "who").c_str(), var("keeper", "beat").c_str(), var("keeper", "loaded").c_str());
        CHECK(var("keeper", "best") == "42" && var("keeper", "who") == "ana" && var("keeper", "beat") == "true");
        CHECK(var("keeper", "loaded") == "1");
        // ANOTHER graph naming the store shares it; one holding `best` as text is not forced.
        CHECK(load_graph(R"({ "graph": 1, "name": "reader", "vars": { "best": 0, "who": 7 },
          "nodes": [ { "id": "c", "type": "on_call", "name": "peek" }, { "id": "l", "type": "load_vars", "store": "progress" } ],
          "links": [ ["c.then", "l.in"] ] })"));
        CHECK(run("graphs.call('peek', dnp.players()[1], nil)", "peek"));
        CHECK(var("reader", "best") == "42");
        CHECK(var("reader", "who") == "7");            // text into a number: left alone
        // A save of SOME variables writes beside the rest, not over them.
        CHECK(load_graph(R"({ "graph": 1, "name": "partial", "vars": { "who": "bo" },
          "nodes": [ { "id": "c", "type": "on_call", "name": "rename" }, { "id": "s", "type": "save_vars", "store": "progress" } ],
          "links": [ ["c.then", "s.in"] ] })"));
        CHECK(run("graphs.call('rename', dnp.players()[1], nil)", "rename"));
        CHECK(run("t_s = dnp.store_load('progress'); t_best = t_s.best; t_who = t_s.who", "the file"));
        CHECK(num("t_best") == 42);
        CHECK(run("t_whoq = (t_who == 'bo')", "who") && truthy("t_whoq"));
        // Forgotten: `none` again.
        CHECK(run("graphs.call('reset_progress', dnp.players()[1], nil)", "forget"));
        CHECK(run("graphs.call('resume', dnp.players()[1], nil)", "resume after forgetting"));
        CHECK(var("keeper", "loaded") == "-1");
        // Nowhere to write: `failed`, and said.
        ctx.state_dir = nullptr;
        CHECK(run("graphs.call('win', dnp.players()[1], nil)", "win with no state dir"));
        CHECK(var("keeper", "saved") == "-1");
        ctx.state_dir = &state_dir;
        // A store name is a file name: no path gets through, at load or at run time.
        CHECK(!load_graph(R"({ "graph": 1, "name": "sneak", "vars": { "a": 1 },
          "nodes": [ { "id": "s", "type": "save_vars", "store": "../../etc" } ], "links": [] })"));
        CHECK(run("t_bad, t_why = dnp.store_save('../x', { a = 1 })", "a bad name") && !truthy("t_bad"));
        // And a variable it does not have is refused at load.
        CHECK(!load_graph(R"({ "graph": 1, "name": "typo", "vars": { "best": 1 },
          "nodes": [ { "id": "s", "type": "save_vars", "vars": "bset" } ], "links": [] })"));
        // Only plain values: a table cannot be saved through the binding.
        CHECK(run("t_t = dnp.store_save('x', { a = function() end })", "a function") && !truthy("t_t"));
        std::remove(file.c_str());
        std::remove((state_dir + "/store/x.store").c_str());
        rmdir((state_dir + "/store").c_str());
        rmdir(state_dir.c_str());
        ctx.state_dir = nullptr;
    }

    // ================================================================
    // HOW A PLAYER LOOKS: layers, animation remaps, and the graph nodes.
    std::printf("-- a player's look --\n");
    {
        const u32 pl = player;
        const u16 rev0 = look_rev[pl];
        CHECK(run("local p = dnp.players()[1]\n"
                  "p:set_layer(1, 91); p:set_layer(8, 97)\n"
                  "lk_1, lk_8, lk_2 = p:layer(1), p:layer(8), p:layer(2)\n"
                  "p:set_anim(2, 12); p:set_anim(3, 13); p:set_anim(2, 22)\n"
                  "lk_bad = pcall(function() p:set_layer(9, 1) end) and 1 or 0\n"
                  "lk_name = pcall(function() p:set_layer(2, 'no_such_prop') end) and 1 or 0", "layers and remaps"));
        CHECK(num("lk_1") == 91 && num("lk_8") == 97 && !truthy("lk_2"));
        CHECK(look[pl].layer[0] == 91 && look[pl].layer[7] == 97);
        // Two remaps, the second set of 2 replacing the first.
        CHECK(look[pl].remaps == 2 && look[pl].from[0] == 2 && look[pl].to[0] == 22 && look[pl].from[1] == 3);
        CHECK(look_rev[pl] != rev0);
        CHECK(num("lk_bad") == 0);       // slot 9 is refused
        CHECK(num("lk_name") == 0);      // and a name the catalogue does not have
        // Taking one back; and a change that changes nothing bumps nothing.
        CHECK(run("dnp.players()[1]:set_anim(2, nil)", "a remap taken back"));
        CHECK(look[pl].remaps == 1 && look[pl].from[0] == 3);
        const u16 rev1 = look_rev[pl];
        CHECK(run("dnp.players()[1]:set_layer(1, 91)", "the same layer again"));
        CHECK(look_rev[pl] == rev1);
        // Eight remaps at most.
        CHECK(run("local p = dnp.players()[1]\n"
                  "for s = 20, 26 do p:set_anim(s, s + 100) end\n"
                  "lk_ninth = pcall(function() p:set_anim(40, 41) end) and 1 or 0", "eight remaps"));
        CHECK(look[pl].remaps == 8 && num("lk_ninth") == 0);
        // CONTROL: a monster has no look -- an agent with a kind.
        const u32 mon = world.spawn(fx3{F(5), F(0), F(0)});
        world.agent_mut(mon).kind = 33;
        CHECK(run("for _, e in ipairs(dnp.entities()) do if e:index() ~= dnp.players()[1]:index() then e:set_layer(1, 91) end end", "a monster"));
        CHECK(look[mon].layer[0] == 0);
        world.despawn(mon);
        // The graph nodes, on the event's player.
        look[pl] = proto::Look{};
        CHECK(load_graph(R"({ "graph": 1, "name": "dress",
          "nodes": [ { "id": "go", "type": "on_call", "name": "dress" },
                     { "id": "a", "type": "set_avatar", "avatar": 3 },
                     { "id": "l", "type": "set_layer", "slot": 2, "prop": "" },
                     { "id": "k", "type": "set_animation_for", "state": 2, "to": 14 } ],
          "links": [ ["go.then", "a.in"], ["a.then", "l.in"], ["l.then", "k.in"] ] })"));
        look[pl].layer[1] = 55;
        CHECK(run("graphs.call('dress', dnp.players()[1], nil)", "dressing"));
        std::printf("  graph: avatar %u, layer 2 %u, remap %u -> %u\n", avatar[pl], look[pl].layer[1],
                    look[pl].remaps ? look[pl].from[0] : 0, look[pl].remaps ? look[pl].to[0] : 0);
        CHECK(avatar[pl] == 3);
        CHECK(look[pl].layer[1] == 0);                 // an empty prop takes the slot's off
        CHECK(look[pl].remaps == 1 && look[pl].from[0] == 2 && look[pl].to[0] == 14);
    }

    // ================================================================
    // CHANGE ONE PLAYER SETTING: chosen from the player's settings only; set,
    // add or multiply; for good or for a while.
    std::printf("-- change_player_setting --\n");
    {
        CHECK(load_graph(R"({ "graph": 1, "name": "tune",
          "nodes": [ { "id": "go", "type": "on_call", "name": "tune" },
                     { "id": "a", "type": "change_player_setting", "setting": "jump_impulse", "value": 12 },
                     { "id": "b", "type": "change_player_setting", "setting": "max_jumps", "value": 1, "how": "add" },
                     { "id": "c", "type": "change_player_setting", "setting": "move_speed", "value": 2,
                       "how": "multiply", "seconds": 1 } ],
          "links": [ ["go.then", "a.in"], ["a.then", "b.in"], ["b.then", "c.in"] ] })"));
        const physics::MoveParams before = effects.compute(player);
        CHECK(run("graphs.call('tune', dnp.players()[1], nil)", "tuning"));
        ticks(2);
        const physics::MoveParams during = effects.compute(player);
        ticks(70);
        effects.expire(g_world->tick());   // as the server does every tick; this harness does not
        const physics::MoveParams later = effects.compute(player);
        std::printf("  jump_impulse %g -> %g, max_jumps %d -> %d, move_speed %g -> %g, a second on %g\n",
                    before.jump_impulse.raw / 4294967296.0, during.jump_impulse.raw / 4294967296.0,
                    before.max_jumps, during.max_jumps, before.move_speed.raw / 4294967296.0,
                    during.move_speed.raw / 4294967296.0, later.move_speed.raw / 4294967296.0);
        CHECK(during.jump_impulse == F(12));                                  // set
        CHECK(during.max_jumps == before.max_jumps + 1);                      // add
        CHECK(during.move_speed == before.move_speed * F(2));                 // multiply...
        CHECK(later.move_speed == before.move_speed);                         // ...for a second
        CHECK(later.jump_impulse == F(12));                                   // CONTROL: a set with no seconds stays
        // CONTROL: a setting that is not a player setting is refused at load.
        CHECK(!load_graph(R"({ "graph": 1, "name": "tunebad",
          "nodes": [ { "id": "go", "type": "on_call", "name": "x" },
                     { "id": "a", "type": "change_player_setting", "setting": "hp", "value": 5 } ],
          "links": [ ["go.then", "a.in"] ] })"));
    }

    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
