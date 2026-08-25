#pragma once
//
// The fx type, as seen from Lua.
//
// This exists because of one fact about the runtime: **LuaJIT is Lua 5.1, and
// every number in it is an IEEE double.** The simulation is Q32.32 fixed point,
// and the whole engine rests on both sides computing bit-identical answers. A
// binding that let a Lua number become an `fx` by rounding would put the one
// conversion the physics contract forbids at the exact boundary scripts live on.
//
// Be precise about WHERE the loss is, because the obvious story is wrong.
//
// A single small value survives the round trip intact: Q32.32 resolves to
// 2^-32 (~2.3e-10) while a double near 1 is exact to ~2.2e-16, so
// `fx.approx(1/3)` and `fx.frac(1, 3)` are the SAME number. The danger is not
// per-value conversion error. It is:
//
//   - **Accumulation.** Ten additions of 0.1 in doubles drift past 2^-32 and
//     the error then survives the conversion. The engine adding fx::from_frac
//     (1, 10) ten times lands somewhere else -- and note it lands somewhere
//     else than 1.0 too, because one tenth is not exact in Q32.32 either.
//     Exactness is not the goal; AGREEING WITH THE ENGINE is. A script using
//     fx produces values the simulation could have produced itself.
//   - **Magnitude.** Above about 2^20 a double's ulp exceeds 2^-32, so large
//     coordinates lose precision the fixed-point format still has.
//   - **Reproducibility of the expression.** LuaJIT may contract multiply-add,
//     so the same source can give a different last bit depending on whether it
//     was interpreted or compiled. (The JIT is off here for budget reasons --
//     see vm.cpp -- but relying on that for CORRECTNESS would make an
//     unrelated performance decision load-bearing.) `math.sin` and friends are
//     libm, which varies by platform and libc version; that is exactly why
//     core/ carries its own CORDIC.
//
// So `fx` is a userdata holding one i64 raw, and arithmetic on it goes through
// core's own operators. Scripts inherit the same truncate-toward-zero rounding
// the physics uses, which is a second benefit: a script-computed value rounds
// the way the engine rounds.
//
// The constructors are the only doors a Lua number walks through, and only one
// of them is lossy:
//
//   fx.int(n)      exact; errors unless n is integer-valued
//   fx.frac(n, d)  exact rational, straight to fx::from_frac
//   fx.parse("8.25")  decimal string, parsed in INTEGER arithmetic
//   fx.raw("...")  round-trip of a raw value
//   fx.approx(n)   the ONLY place a non-integral double crosses. Lossy, and
//                  deliberately greppable: one search over a map's scripts
//                  lists every place inexactness entered.
//
// Anything else that takes an fx accepts an fx userdata, an integer-valued
// number, or a decimal string, and raises a LOUD error naming the value
// otherwise -- so `set_speed(8.3)` fails at the call site instead of silently
// quantising.
//
#include <cstddef>

#include "dnp/math/fixed.hpp"
#include "dnp/math/vec.hpp"

struct lua_State;

namespace dnp::server::lua {

using namespace dnp;

// NOTE ON noexcept, because getting this wrong crashes the server.
//
// LuaJIT raises errors by UNWINDING -- on this platform through the C++
// exception machinery. Any function that can call luaL_error(), or any Lua API
// that can raise (lua_newuserdata can, on allocation failure), therefore
// propagates an exception. Marking such a function `noexcept` does not make it
// not throw; it makes the throw call std::terminate. The first version of this
// file did exactly that and died with "terminating due to uncaught foreign
// exception" the moment a script passed a bad argument -- i.e. the server would
// have crashed on the first script typo in production.
//
// So nothing here that touches the Lua stack is noexcept. fx_from_decimal is,
// because it is pure integer parsing and never enters Lua.

// Install the `fx` and `vec3` types and their constructors as globals.
void open_fx(lua_State* L);

// Push a value.
void push_fx(lua_State* L, fx v);
void push_vec3(lua_State* L, fx3 v);

// Read an argument, accepting an fx userdata, an integer-valued number, or a
// decimal string. Raises a Lua error otherwise; does not return on failure.
fx  check_fx(lua_State* L, int idx);
fx3 check_vec3(lua_State* L, int idx);

// Non-raising variants for callers that want to branch.
bool is_fx(lua_State* L, int idx);
bool is_vec3(lua_State* L, int idx);

// Parse a decimal string exactly, in integer arithmetic. Returns false on
// anything it cannot represent exactly -- including an exponent, which is a
// double's notation and not this type's.
bool fx_from_decimal(const char* s, size_t len, fx& out) noexcept;

}  // namespace dnp::server::lua
