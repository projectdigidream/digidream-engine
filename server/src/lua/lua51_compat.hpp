// What LuaJIT has from Lua 5.2 and PUC Lua 5.1 does not, for the web build's
// server (scripts/setup_lua51.sh, DNP_LUA=lua51). Included after lauxlib.h.
// Under LuaJIT -- its headers on the include path -- this is nothing.
#pragma once

#if !__has_include("luajit.h")
inline void* luaL_testudata(lua_State* L, int i, const char* tname) {
    void* p = lua_touserdata(L, i);
    if (p == nullptr || !lua_getmetatable(L, i)) return nullptr;
    luaL_getmetatable(L, tname);
    const bool same = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    return same ? p : nullptr;
}
#endif
