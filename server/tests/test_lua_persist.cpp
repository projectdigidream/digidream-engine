//
// Persistence, the Lua side (docs/SAVING.md): what a script library saves
// through dnp.persist and gets back, against the real bindings.
//
// The encoding is the contract every saved level and every save game rests
// on, so it is tested for the three things that would quietly break them:
// that what goes in comes out exactly (fx by its raw value -- a Doom unit is
// 1/48, which no decimal says exactly); that the same state always encodes to
// the same bytes; and that what cannot mean anything later -- a function, a
// handle -- is refused at save time, by name, rather than saved as nonsense.
//
#include <cstdio>
#include <string>

#include "dnp/sim/testmap.hpp"
#include "dnp/sim/world.hpp"
#include "lua/vm.hpp"
#include "lua/world_binding.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

using namespace dnp;
using namespace dnp::server;

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

static lua_State* g_L = nullptr;

static bool run(const char* code) {
    if (luaL_dostring(g_L, code) != 0) {
        std::printf("  lua error: %s\n", lua_tostring(g_L, -1));
        lua_pop(g_L, 1);
        return false;
    }
    return true;
}

// Encode the global `name`.
static std::string enc(const char* name, std::string* err = nullptr) {
    lua_getglobal(g_L, name);
    std::string out, e;
    const bool ok = lua::encode_value(g_L, -1, out, e);
    lua_pop(g_L, 1);
    if (err) *err = e;
    else if (!ok) std::printf("  (encoding %s refused: %s)\n", name, e.c_str());
    return ok ? out : std::string();
}

static void test_round_trip() {
    std::printf("persist: what is saved comes back exactly\n");
    CHECK(run("v = { name = 'imp', hp = 60, alive = true, dead = false,"
              "      at = vec3.new(fx.frac(17, 48), fx.frac(-5, 48), fx.int(2)),"
              "      speed = fx.frac(1, 3), ratio = 0.1, big = 2^53,"
              "      list = { 'a', 'b', { 1, 2, 3 } }, [7] = 'seven',"
              "      bytes = 'tab\\tnl\\nnul\\0end' }"));
    const std::string a = enc("v");
    CHECK(!a.empty());
    // Back in, and out again: the same bytes.
    std::string err;
    CHECK(lua::decode_value(g_L, a, err));
    lua_setglobal(g_L, "w");
    CHECK(enc("w") == a);
    // And the values themselves, through Lua.
    CHECK(run("assert(w.name == 'imp' and w.hp == 60 and w.alive == true and w.dead == false)"));
    CHECK(run("assert(w.at.x == fx.frac(17, 48) and w.at.y == fx.frac(-5, 48) and w.at.z == fx.int(2))"));
    CHECK(run("assert(w.speed == fx.frac(1, 3))"));       // exact, not 0.333...
    CHECK(run("assert(w.ratio == 0.1 and w.big == 2^53)"));
    CHECK(run("assert(w.list[3][2] == 2 and w[7] == 'seven')"));
    CHECK(run("assert(w.bytes == 'tab\\tnl\\nnul\\0end')"));
}

static void test_same_state_same_bytes() {
    std::printf("persist: the same state encodes to the same bytes\n");
    CHECK(run("p = {}; p.b = 2; p.a = 1; p.c = { z = 1, y = 2 }"));
    CHECK(run("q = {}; q.c = { y = 2, z = 1 }; q.a = 1; q.b = 2"));
    CHECK(enc("p") == enc("q"));
    // CONTROL: a different value is a different encoding.
    CHECK(run("r = { a = 1, b = 3, c = { y = 2, z = 1 } }"));
    CHECK(enc("p") != enc("r"));
}

static void test_refused_by_name() {
    std::printf("persist: what would mean nothing later is refused, by name\n");
    std::string err;
    CHECK(run("bad = { monsters = { { kind = 'imp', think = function() end } } }"));
    CHECK(enc("bad", &err).empty());
    CHECK(err.find("function") != std::string::npos && err.find("monsters") != std::string::npos);
    CHECK(run("me = { who = dnp.entity(0) }"));
    CHECK(enc("me", &err).empty());
    CHECK(err.find("handle") != std::string::npos);
    CHECK(run("loop = {}; loop.self = loop"));
    CHECK(enc("loop", &err).empty());
    CHECK(err.find("deep") != std::string::npos);
    // Malformed save data is refused too, and pushes nothing.
    const int top = lua_gettop(g_L);
    CHECK(!lua::decode_value(g_L, "{s5:ab", err));
    CHECK(!lua::decode_value(g_L, "i12", err));
    CHECK(lua_gettop(g_L) == top);
}

static void test_parts_save_and_load(lua::WorldContext& ctx) {
    std::printf("persist: a library's state goes out and comes back through its hooks\n");
    CHECK(run("doors = { [3] = 'open', [9] = 'shut' }; keys = { 'blue' }"
              "dnp.persist('doors', function() return doors end,"
              "                     function(t) doors = t end)"
              "dnp.persist('keys', function() return keys end,"
              "                    function(t) keys = t end)"
              "dnp.persist('broken', function() error('no') end, function() end)"));
    std::string saved, err;
    CHECK(lua::persist_capture(g_L, saved, err));
    // The broken part is named and the others kept.
    CHECK(err.find("broken") != std::string::npos);
    CHECK(saved.find("doors") != std::string::npos && saved.find("keys") != std::string::npos);

    CHECK(run("doors = nil; keys = nil"));
    err.clear();
    CHECK(lua::persist_restore(g_L, saved, err));
    CHECK(err.empty());
    CHECK(run("assert(doors[3] == 'open' and doors[9] == 'shut' and keys[1] == 'blue')"));

    // A part nothing loads any more -- a library removed since the save -- is
    // named, not fatal.
    std::string old = "{s5:ghost{}}";
    err.clear();
    CHECK(lua::persist_restore(g_L, old, err));
    CHECK(err.find("ghost") != std::string::npos);

    // dnp.restoring() says what the context says.
    ctx.restoring = true;
    CHECK(run("assert(dnp.restoring() == true)"));
    ctx.restoring = false;
    CHECK(run("assert(dnp.restoring() == false)"));
}

static void test_player_parts(lua::WorldContext& ctx, u32 agent) {
    std::printf("persist: a player's own state, through the player's hooks\n");
    CHECK(run("inv = {}"
              "dnp.persist_player('inventory',"
              "  function(e) return inv[e:index()] end,"
              "  function(e, t) inv[e:index()] = t end)"));
    char code[160];
    std::snprintf(code, sizeof(code), "inv[%u] = { shells = 8, weapons = { 'shotgun' } }", agent);
    CHECK(run(code));
    std::string saved, err;
    CHECK(lua::persist_player_capture(g_L, ctx, agent, saved, err));
    CHECK(run("inv = {}"));
    CHECK(lua::persist_player_restore(g_L, ctx, agent, saved, err));
    std::snprintf(code, sizeof(code), "assert(inv[%u].shells == 8 and inv[%u].weapons[1] == 'shotgun')",
                  agent, agent);
    CHECK(run(code));
}

int main() {
    static sim::World world;
    world.set_map(&sim::test_map());
    const u32 agent = world.spawn(fx3{fx::from_int(10), fx::from_int(10), fx::from_int(1)});

    static i32 hp[sim::MAX_AGENTS];
    static lua::CommandQueue commands;
    static lua::EffectStack effects;
    static lua::WorldContext ctx;
    ctx.world = &world;
    ctx.hp = hp;
    ctx.commands = &commands;
    ctx.effects = &effects;

    static lua::Vm vm;
    lua::VmConfig cfg;
    if (!vm.open(cfg)) { std::printf("  FAIL vm.open\n"); return 1; }
    lua::open_world(vm.state(), &ctx);
    g_L = vm.state();

    test_round_trip();
    test_same_state_same_bytes();
    test_refused_by_name();
    test_parts_save_and_load(ctx);
    test_player_parts(ctx, agent);

    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
