#pragma once
//
// JSON to a Lua value, for data a script is handed rather than reads: a graph
// (docs/GRAPHS.md) is decoded here and passed to graphs.load, so no script
// needs a way to open files -- under strict trust there is none.
//
// Strict RFC 8259 apart from one refusal: `null`, which a Lua table cannot
// hold in an array without a hole that silently shortens it. Objects become
// tables keyed by string, arrays 1-based tables, numbers Lua numbers.
//
#include <cstddef>
#include <string>

struct lua_State;

namespace dnp::server::lua {

// Push the value `text` encodes, or push nothing and say why. Depth is capped
// so a hostile file cannot overflow the C stack.
bool push_json(lua_State* L, const char* text, size_t len, std::string& err);

// Read a file and push its value: what start_scripts uses for a .graph.json.
bool push_json_file(lua_State* L, const std::string& path, std::string& err);

}  // namespace dnp::server::lua
