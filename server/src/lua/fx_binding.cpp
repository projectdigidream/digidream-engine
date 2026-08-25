#include "lua/fx_binding.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace dnp::server::lua {

namespace {

const char* const FX_MT   = "dnp.fx";
const char* const VEC3_MT = "dnp.vec3";

fx* new_fx(lua_State* L) {
    void* p = lua_newuserdata(L, sizeof(fx));
    luaL_getmetatable(L, FX_MT);
    lua_setmetatable(L, -2);
    return static_cast<fx*>(p);
}

fx3* new_vec3(lua_State* L) {
    void* p = lua_newuserdata(L, sizeof(fx3));
    luaL_getmetatable(L, VEC3_MT);
    lua_setmetatable(L, -2);
    return static_cast<fx3*>(p);
}

fx* to_fx(lua_State* L, int idx) {
    return static_cast<fx*>(luaL_testudata(L, idx, FX_MT));
}

fx3* to_vec3(lua_State* L, int idx) {
    return static_cast<fx3*>(luaL_testudata(L, idx, VEC3_MT));
}

// --------------------------------------------------------------- constructors

int l_fx_int(lua_State* L) {
    const lua_Number n = luaL_checknumber(L, 1);
    // Integer-valued is checked rather than assumed. A script writing
    // fx.int(8.5) has made a mistake, and rounding it silently is how a
    // half-unit of speed goes unexplained for a week.
    if (n != std::floor(n) || std::fabs(n) > 2147483647.0) {
        return luaL_error(L, "fx.int(%f): not an exact integer in range; "
                             "use fx.frac(n, d) or fx.parse(\"...\")", n);
    }
    push_fx(L, fx::from_int(static_cast<i64>(n)));
    return 1;
}

int l_fx_frac(lua_State* L) {
    const lua_Number n = luaL_checknumber(L, 1);
    const lua_Number d = luaL_checknumber(L, 2);
    if (n != std::floor(n) || d != std::floor(d)) {
        return luaL_error(L, "fx.frac(%f, %f): both parts must be integers", n, d);
    }
    if (d == 0) return luaL_error(L, "fx.frac: division by zero");
    push_fx(L, fx::from_frac(static_cast<i64>(n), static_cast<i64>(d)));
    return 1;
}

int l_fx_parse(lua_State* L) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    fx out{};
    if (!fx_from_decimal(s, len, out)) {
        return luaL_error(L, "fx.parse('%s'): not an exact decimal", s);
    }
    push_fx(L, out);
    return 1;
}

int l_fx_raw(lua_State* L) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    // Through a string, because a Q32.32 raw exceeds the 2^53 a double can hold
    // exactly. Taking it as a number would silently round the very value whose
    // whole purpose is to be exact.
    char* end = nullptr;
    const long long v = std::strtoll(s, &end, 10);
    if (end == s || (end && *end != '\0')) {
        return luaL_error(L, "fx.raw('%s'): not an integer", s);
    }
    push_fx(L, fx::from_raw(static_cast<i64>(v)));
    return 1;
}

int l_fx_approx(lua_State* L) {
    const lua_Number n = luaL_checknumber(L, 1);
    // The one lossy door, and it is named so that it is greppable. Everything
    // else in this file exists to make sure a value did not arrive here by
    // accident.
    const double scaled = static_cast<double>(n) * 4294967296.0;
    if (!(scaled >= -9.22e18 && scaled <= 9.22e18)) {
        return luaL_error(L, "fx.approx(%f): out of range", n);
    }
    push_fx(L, fx::from_raw(static_cast<i64>(scaled)));
    return 1;
}

// ---------------------------------------------------------------- metamethods

int l_fx_add(lua_State* L) { push_fx(L, check_fx(L, 1) + check_fx(L, 2)); return 1; }
int l_fx_sub(lua_State* L) { push_fx(L, check_fx(L, 1) - check_fx(L, 2)); return 1; }
int l_fx_mul(lua_State* L) { push_fx(L, check_fx(L, 1) * check_fx(L, 2)); return 1; }

int l_fx_div(lua_State* L) {
    const fx a = check_fx(L, 1);
    const fx b = check_fx(L, 2);
    if (b.raw == 0) return luaL_error(L, "fx division by zero");
    push_fx(L, a / b);
    return 1;
}

int l_fx_unm(lua_State* L) { push_fx(L, -check_fx(L, 1)); return 1; }
int l_fx_eq(lua_State* L)  { lua_pushboolean(L, check_fx(L, 1) == check_fx(L, 2)); return 1; }
int l_fx_lt(lua_State* L)  { lua_pushboolean(L, check_fx(L, 1) <  check_fx(L, 2)); return 1; }
int l_fx_le(lua_State* L)  { lua_pushboolean(L, check_fx(L, 1) <= check_fx(L, 2)); return 1; }

int l_fx_tostring(lua_State* L) {
    const fx v = check_fx(L, 1);
    // Rendered from the integer parts, never by dividing into a double: a
    // printed value that disagrees with the stored one by a rounding step is
    // how a correct number gets debugged for an afternoon.
    const bool neg = v.raw < 0;
    const u64 mag = neg ? static_cast<u64>(-(v.raw + 1)) + 1u : static_cast<u64>(v.raw);
    const u64 whole = mag >> 32;
    const u64 frac = mag & 0xFFFFFFFFull;
    // Six decimal places, computed in integers.
    const u64 micros = (frac * 1000000ull) >> 32;
    lua_pushfstring(L, "%s%d.%06d", neg ? "-" : "", static_cast<int>(whole),
                    static_cast<int>(micros));
    return 1;
}

int l_fx_index(lua_State* L) {
    const fx v = check_fx(L, 1);
    const char* k = luaL_checkstring(L, 2);
    if (std::strcmp(k, "raw") == 0) {
        // As a string: the raw exceeds 2^53 and a double would round it.
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v.raw));
        lua_pushstring(L, buf);
        return 1;
    }
    if (std::strcmp(k, "int") == 0) {
        lua_pushnumber(L, static_cast<lua_Number>(v.raw >> 32));
        return 1;
    }
    return luaL_error(L, "fx has no field '%s'", k);
}

// ---------------------------------------------------------------------- vec3

int l_vec3_new(lua_State* L) {
    push_vec3(L, fx3{check_fx(L, 1), check_fx(L, 2), check_fx(L, 3)});
    return 1;
}

int l_vec3_add(lua_State* L) { push_vec3(L, check_vec3(L, 1) + check_vec3(L, 2)); return 1; }
int l_vec3_sub(lua_State* L) { push_vec3(L, check_vec3(L, 1) - check_vec3(L, 2)); return 1; }
int l_vec3_unm(lua_State* L) { push_vec3(L, -check_vec3(L, 1)); return 1; }

int l_vec3_eq(lua_State* L) {
    lua_pushboolean(L, check_vec3(L, 1) == check_vec3(L, 2));
    return 1;
}

int l_vec3_index(lua_State* L) {
    const fx3 v = check_vec3(L, 1);
    const char* k = luaL_checkstring(L, 2);
    if (std::strcmp(k, "x") == 0) { push_fx(L, v.x); return 1; }
    if (std::strcmp(k, "y") == 0) { push_fx(L, v.y); return 1; }
    if (std::strcmp(k, "z") == 0) { push_fx(L, v.z); return 1; }
    return luaL_error(L, "vec3 has no field '%s'", k);
}

int l_vec3_tostring(lua_State* L) {
    const fx3 v = check_vec3(L, 1);
    push_fx(L, v.x); lua_getglobal(L, "tostring"); lua_insert(L, -2); lua_call(L, 1, 1);
    push_fx(L, v.y); lua_getglobal(L, "tostring"); lua_insert(L, -2); lua_call(L, 1, 1);
    push_fx(L, v.z); lua_getglobal(L, "tostring"); lua_insert(L, -2); lua_call(L, 1, 1);
    lua_pushfstring(L, "(%s, %s, %s)", lua_tostring(L, -3), lua_tostring(L, -2),
                    lua_tostring(L, -1));
    return 1;
}

}  // namespace

// ------------------------------------------------------------------- decimal

bool fx_from_decimal(const char* s, size_t len, fx& out) noexcept {
    if (!s || len == 0) return false;

    size_t i = 0;
    bool neg = false;
    if (s[i] == '+' || s[i] == '-') { neg = s[i] == '-'; ++i; }
    if (i >= len) return false;

    i64 whole = 0;
    bool any = false;
    for (; i < len && s[i] >= '0' && s[i] <= '9'; ++i) {
        if (whole > 2000000000ll) return false;   // out of Q32.32's integral range
        whole = whole * 10 + (s[i] - '0');
        any = true;
    }

    fx frac_part = fx::zero();
    if (i < len && s[i] == '.') {
        ++i;
        i64 num = 0;
        i64 den = 1;
        for (; i < len && s[i] >= '0' && s[i] <= '9'; ++i) {
            // Ten digits is past Q32.32's resolution; refusing beats pretending.
            if (den > 1000000000ll) return false;
            num = num * 10 + (s[i] - '0');
            den *= 10;
            any = true;
        }
        // from_frac is exact to one ulp and evaluated in 128-bit -- the same
        // path a C++ literal like fx::from_frac(25, 100) takes.
        frac_part = fx::from_frac(num, den);
    }

    // No exponent. `1e3` is a double's notation, and accepting it here would
    // invite the very "it looked like a number so I passed it" habit this type
    // exists to prevent.
    if (!any || i != len) return false;

    const fx v = fx::from_int(whole) + frac_part;
    out = neg ? -v : v;
    return true;
}

// --------------------------------------------------------------------- checks

bool is_fx(lua_State* L, int idx) { return to_fx(L, idx) != nullptr; }
bool is_vec3(lua_State* L, int idx) { return to_vec3(L, idx) != nullptr; }

void push_fx(lua_State* L, fx v) { *new_fx(L) = v; }
void push_vec3(lua_State* L, fx3 v) { *new_vec3(L) = v; }

fx check_fx(lua_State* L, int idx) {
    if (fx* p = to_fx(L, idx)) return *p;

    if (lua_type(L, idx) == LUA_TSTRING) {
        size_t len = 0;
        const char* s = lua_tolstring(L, idx, &len);
        fx out{};
        if (fx_from_decimal(s, len, out)) return out;
        luaL_error(L, "expected fx, got the string '%s' which is not an exact decimal", s);
        return fx::zero();
    }

    if (lua_type(L, idx) == LUA_TNUMBER) {
        const lua_Number n = lua_tonumber(L, idx);
        if (n == std::floor(n) && std::fabs(n) <= 2147483647.0) {
            // An integer-valued number is exact in a double and exact as an fx,
            // so it converts silently. Nothing is lost and requiring fx.int(2)
            // everywhere would be noise.
            return fx::from_int(static_cast<i64>(n));
        }
        // The loud failure this whole file exists for. Naming the value is the
        // point: "expected fx" alone sends the author looking at the wrong line.
        luaL_error(L,
                   "expected fx, got the non-integral number %f. Doubles are not "
                   "exact in Q32.32 -- use fx.frac(n, d), fx.parse(\"%f\"), or "
                   "fx.approx(...) if you accept the rounding",
                   n, n);
        return fx::zero();
    }

    luaL_error(L, "expected fx, got %s", luaL_typename(L, idx));
    return fx::zero();
}

fx3 check_vec3(lua_State* L, int idx) {
    if (fx3* p = to_vec3(L, idx)) return *p;
    luaL_error(L, "expected vec3, got %s", luaL_typename(L, idx));
    return fx3::zero();
}

// ---------------------------------------------------------------------- open

void open_fx(lua_State* L) {
    luaL_newmetatable(L, FX_MT);
    lua_pushcfunction(L, l_fx_add);      lua_setfield(L, -2, "__add");
    lua_pushcfunction(L, l_fx_sub);      lua_setfield(L, -2, "__sub");
    lua_pushcfunction(L, l_fx_mul);      lua_setfield(L, -2, "__mul");
    lua_pushcfunction(L, l_fx_div);      lua_setfield(L, -2, "__div");
    lua_pushcfunction(L, l_fx_unm);      lua_setfield(L, -2, "__unm");
    lua_pushcfunction(L, l_fx_eq);       lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, l_fx_lt);       lua_setfield(L, -2, "__lt");
    lua_pushcfunction(L, l_fx_le);       lua_setfield(L, -2, "__le");
    lua_pushcfunction(L, l_fx_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, l_fx_index);    lua_setfield(L, -2, "__index");
    // Opaque, so a script cannot reach in and rewrite the arithmetic every
    // other script depends on.
    lua_pushliteral(L, "locked");        lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    luaL_newmetatable(L, VEC3_MT);
    lua_pushcfunction(L, l_vec3_add);      lua_setfield(L, -2, "__add");
    lua_pushcfunction(L, l_vec3_sub);      lua_setfield(L, -2, "__sub");
    lua_pushcfunction(L, l_vec3_unm);      lua_setfield(L, -2, "__unm");
    lua_pushcfunction(L, l_vec3_eq);       lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, l_vec3_index);    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_vec3_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushliteral(L, "locked");          lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, l_fx_int);    lua_setfield(L, -2, "int");
    lua_pushcfunction(L, l_fx_frac);   lua_setfield(L, -2, "frac");
    lua_pushcfunction(L, l_fx_parse);  lua_setfield(L, -2, "parse");
    lua_pushcfunction(L, l_fx_raw);    lua_setfield(L, -2, "raw");
    lua_pushcfunction(L, l_fx_approx); lua_setfield(L, -2, "approx");
    lua_setglobal(L, "fx");

    lua_newtable(L);
    lua_pushcfunction(L, l_vec3_new); lua_setfield(L, -2, "new");
    lua_setglobal(L, "vec3");
}

}  // namespace dnp::server::lua
