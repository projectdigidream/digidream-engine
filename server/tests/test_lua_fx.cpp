//
// The fx type as seen from Lua.
//
// LuaJIT is Lua 5.1: every number in it is an IEEE double, and the simulation
// is Q32.32 fixed point whose entire premise is that both sides compute
// bit-identical answers. This file is the boundary between those two facts.
//
// The test that matters most is not the round trip -- it is that the EXACT
// constructors and the lossy one produce different values. If they agreed, the
// exact path would be decoration and nobody would notice until a script author
// wrote 1/3 and a client started mispredicting.
//
#include <cstdio>
#include <cstring>

#include "lua/fx_binding.hpp"
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

static bool run(Vm& vm, const char* src) {
    lua_State* L = vm.state();
    vm.begin_tick();
    if (luaL_loadbuffer(L, src, std::strlen(src), "=probe") != 0) { lua_pop(L, 1); return false; }
    const bool ok = lua_pcall(L, 0, 0, 0) == 0;
    if (!ok) lua_pop(L, 1);
    return ok;
}

// Evaluate an expression yielding an fx and return its raw.
static i64 eval_raw(Vm& vm, const char* expr) {
    lua_State* L = vm.state();
    char buf[512];
    std::snprintf(buf, sizeof(buf), "return (%s)", expr);
    vm.begin_tick();
    if (luaL_loadbuffer(L, buf, std::strlen(buf), "=eval") != 0) { lua_pop(L, 1); return 0; }
    if (lua_pcall(L, 0, 1, 0) != 0) { lua_pop(L, 1); return 0; }
    const i64 r = is_fx(L, -1) ? check_fx(L, -1).raw : 0;
    lua_pop(L, 1);
    return r;
}

static void test_exact_constructors() {
    std::printf("fx: the exact constructors match C++ bit for bit\n");
    Vm vm; VmConfig cfg; CHECK(vm.open(cfg));

    CHECK(eval_raw(vm, "fx.int(8)") == fx::from_int(8).raw);
    CHECK(eval_raw(vm, "fx.int(-3)") == fx::from_int(-3).raw);
    CHECK(eval_raw(vm, "fx.frac(1, 3)") == fx::from_frac(1, 3).raw);
    CHECK(eval_raw(vm, "fx.frac(-45, 100)") == fx::from_frac(-45, 100).raw);

    // The decimal parser goes through from_frac, so it lands on exactly what
    // the equivalent C++ literal would.
    CHECK(eval_raw(vm, "fx.parse('8.25')") == (fx::from_int(8) + fx::from_frac(25, 100)).raw);
    CHECK(eval_raw(vm, "fx.parse('-0.45')") == (-fx::from_frac(45, 100)).raw);
    CHECK(eval_raw(vm, "fx.parse('12')") == fx::from_int(12).raw);

    // Raw values travel as strings: a Q32.32 raw exceeds the 2^53 a double
    // holds exactly, so taking it as a number would round the one thing whose
    // entire purpose is not to be rounded.
    CHECK(eval_raw(vm, "fx.raw('4294967296')") == fx::from_int(1).raw);
    std::printf("       int, frac, parse and raw all exact\n");
}

// THE control -- and the shape of it is a correction to my own reasoning.
//
// The obvious version compared fx.approx(1/3) against fx.frac(1,3) and FAILED,
// because they are identical. Q32.32's resolution is 2^-32 (~2.3e-10) and a
// double near 1 is exact to ~2.2e-16, so for a single small value the double
// round-trip lands on exactly the same fixed-point number. The premise that
// "1/3 in a double gives you a value nobody chose" is true about the double and
// false about the conversion.
//
// Where it genuinely bites is ACCUMULATED arithmetic -- which is what a script
// author actually writes. Ten additions of 0.1 in doubles do not make 1.0, and
// once the error has grown past 2^-32 it survives the conversion.
static void test_approx_really_is_different() {
    std::printf("fx: CONTROL -- accumulated double arithmetic does NOT survive\n");
    Vm vm; VmConfig cfg; CHECK(vm.open(cfg));

    // The exact path: ten tenths is one, and it is one exactly.
    const i64 exact = eval_raw(vm,
        "(function() local s = fx.int(0) "
        "for i = 1, 10 do s = s + fx.frac(1, 10) end return s end)()");

    // The lossy path: the same sum in doubles, converted at the end.
    const i64 approx = eval_raw(vm,
        "(function() local s = 0 "
        "for i = 1, 10 do s = s + 0.1 end return fx.approx(s) end)()");

    // What the ENGINE would compute for the same sum.
    //
    // Note it is not 1.0. One tenth is not representable in Q32.32 any more
    // than it is in binary floating point, so from_frac truncates and ten of
    // them land six ulp short. The exact constructors do not make 0.1 exact --
    // nothing can. What they buy is AGREEMENT WITH THE ENGINE, which is the
    // only property prediction actually needs: a script using fx.frac produces
    // a value the simulation could have produced itself, and a script using
    // doubles produces one it never would.
    fx engine = fx::zero();
    for (int i = 0; i < 10; ++i) engine += fx::from_frac(1, 10);

    CHECK(exact == engine.raw);    // the script matches the engine, bit for bit
    CHECK(approx != engine.raw);   // the double path does not
    CHECK(exact != approx);
    std::printf("       engine=%lld  script-exact=%lld  script-approx=%lld\n",
                static_cast<long long>(engine.raw), static_cast<long long>(exact),
                static_cast<long long>(approx));
    std::printf("       (neither is 1.0 -- %lld -- because 1/10 is not exact in "
                "Q32.32 either)\n", static_cast<long long>(fx::from_int(1).raw));

    // And the honest other half: for a single small value the two DO agree.
    // Recorded so nobody later "fixes" fx.approx to chase a difference that is
    // not there, and so the argument for the exact path is the real one --
    // accumulation and magnitude, not per-value conversion error.
    CHECK(eval_raw(vm, "fx.approx(1/3)") == eval_raw(vm, "fx.frac(1, 3)"));
    std::printf("       (and fx.approx(1/3) == fx.frac(1,3): Q32.32 is coarser "
                "than a double near 1)\n");
}

static void test_a_non_integral_number_is_refused_loudly() {
    std::printf("fx: a non-integral double is refused at the call site\n");
    Vm vm; VmConfig cfg; CHECK(vm.open(cfg));

    // The failure this type exists for: set_speed(8.3) must not silently
    // quantise. Arithmetic against a bare 8.3 has to fail.
    CHECK(!run(vm, "local a = fx.int(1) + 8.3"));
    CHECK(!run(vm, "return fx.int(8.5)"));

    // ...and the error must NAME the value, or the author goes looking at the
    // wrong line.
    lua_State* L = vm.state();
    vm.begin_tick();
    luaL_loadstring(L, "return fx.int(1) + 8.3");
    lua_pcall(L, 0, 1, 0);
    const char* msg = lua_tostring(L, -1);
    CHECK(msg != nullptr && std::strstr(msg, "8.3") != nullptr);
    CHECK(msg != nullptr && std::strstr(msg, "fx.frac") != nullptr);
    std::printf("       %s\n", msg ? msg : "(no message)");
    lua_pop(L, 1);

    // An INTEGER-valued number is exact in both representations, so it is
    // accepted silently. Requiring fx.int(2) everywhere would be noise that
    // teaches authors to reach for fx.approx.
    CHECK(run(vm, "if fx.int(1) + 2 ~= fx.int(3) then error('int rejected') end"));
}

static void test_arithmetic_matches_the_engine() {
    std::printf("fx: script arithmetic rounds the way the physics rounds\n");
    Vm vm; VmConfig cfg; CHECK(vm.open(cfg));

    // Multiplication truncates toward zero in core (docs/physics-contract.md
    // rule 4). A script inheriting a different rounding rule would produce
    // values the engine could not have produced itself.
    const fx a = fx::from_frac(1, 3);
    const fx b = fx::from_frac(7, 11);
    CHECK(eval_raw(vm, "fx.frac(1,3) * fx.frac(7,11)") == (a * b).raw);
    CHECK(eval_raw(vm, "fx.frac(1,3) / fx.frac(7,11)") == (a / b).raw);
    CHECK(eval_raw(vm, "-fx.frac(1,3)") == (-a).raw);

    // Mirror symmetry, the property the whole fixed-point design protects.
    CHECK(eval_raw(vm, "(-fx.frac(1,3)) * fx.frac(7,11)") ==
          eval_raw(vm, "-(fx.frac(1,3) * fx.frac(7,11))"));

    CHECK(run(vm, "if not (fx.int(1) < fx.int(2)) then error('lt') end"));
    CHECK(run(vm, "if not (fx.int(2) <= fx.int(2)) then error('le') end"));
    CHECK(run(vm, "if fx.int(2) ~= fx.int(2) then error('eq') end"));
    CHECK(!run(vm, "return fx.int(1) / fx.int(0)"));
}

static void test_tostring_does_not_lie() {
    std::printf("fx: tostring is rendered from integers, not through a double\n");
    Vm vm; VmConfig cfg; CHECK(vm.open(cfg));

    CHECK(run(vm, "if tostring(fx.int(8)) ~= '8.000000' then error(tostring(fx.int(8))) end"));
    CHECK(run(vm, "if tostring(fx.parse('-0.5')) ~= '-0.500000' then "
                  "error(tostring(fx.parse('-0.5'))) end"));

    // The raw is exposed as a string for the same reason it is taken as one.
    CHECK(run(vm, "if fx.int(1).raw ~= '4294967296' then error(fx.int(1).raw) end"));
}

static void test_the_decimal_parser_refuses_what_it_cannot_represent() {
    std::printf("fx: the decimal parser refuses rather than approximates\n");
    Vm vm; VmConfig cfg; CHECK(vm.open(cfg));

    // Exponents are a double's notation. Accepting them would invite exactly
    // the "it looked like a number so I passed it" habit this type prevents.
    CHECK(!run(vm, "return fx.parse('1e3')"));
    CHECK(!run(vm, "return fx.parse('abc')"));
    CHECK(!run(vm, "return fx.parse('')"));
    CHECK(!run(vm, "return fx.parse('1.2.3')"));
    CHECK(!run(vm, "return fx.parse('8 ')"));

    // Beyond Q32.32's resolution, refusing beats pretending.
    CHECK(!run(vm, "return fx.parse('0.12345678901234')"));

    fx out{};
    CHECK(fx_from_decimal("0.5", 3, out) && out == fx::from_frac(1, 2));
    CHECK(!fx_from_decimal("1e3", 3, out));
    CHECK(!fx_from_decimal(nullptr, 0, out));
}

static void test_vec3() {
    std::printf("fx: vec3 composes from fx and nothing else\n");
    Vm vm; VmConfig cfg; CHECK(vm.open(cfg));

    CHECK(run(vm, "local v = vec3.new(fx.int(1), fx.int(2), fx.int(3)) "
                  "if v.x ~= fx.int(1) or v.z ~= fx.int(3) then error('fields') end"));
    CHECK(run(vm, "local a = vec3.new(1, 2, 3) local b = vec3.new(4, 5, 6) "
                  "if (a + b) ~= vec3.new(5, 7, 9) then error('add') end"));
    CHECK(run(vm, "if -vec3.new(1,2,3) ~= vec3.new(-1,-2,-3) then error('unm') end"));
    // The same refusal applies componentwise.
    CHECK(!run(vm, "return vec3.new(1.5, 2, 3)"));
    CHECK(run(vm, "if tostring(vec3.new(1,2,3)) ~= '(1.000000, 2.000000, 3.000000)' then "
                  "error(tostring(vec3.new(1,2,3))) end"));
}

static void test_the_metatable_is_opaque() {
    std::printf("fx: the fx metatable cannot be rewritten by a script\n");
    Vm vm; VmConfig cfg; CHECK(vm.open(cfg));
    // One script repointing __mul would change arithmetic for every other
    // script in the VM, and the symptom would be a physics value that is wrong
    // only sometimes.
    CHECK(run(vm, "if getmetatable(fx.int(1)) ~= 'locked' then error('exposed') end"));
    CHECK(!run(vm, "setmetatable(fx.int(1), {})"));
}

int main() {
    std::printf("== server/lua fx suite ==\n");
    test_exact_constructors();
    test_approx_really_is_different();
    test_a_non_integral_number_is_refused_loudly();
    test_arithmetic_matches_the_engine();
    test_tostring_does_not_lie();
    test_the_decimal_parser_refuses_what_it_cannot_represent();
    test_vec3();
    test_the_metatable_is_opaque();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
