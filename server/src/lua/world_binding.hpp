#pragma once
//
// The world as seen from Lua: entities, and the `dnp` table.
//
// Reads are IMMEDIATE and writes are DEFERRED, and that asymmetry is the whole
// shape of this API. A getter asks the live world; a mutator appends a command
// that lands at the next flush point. So within one callback a script sees the
// world as it was when the callback started, which is what makes the ordering
// of several scripts irrelevant to the result.
//
// Handles are generational (design §8.7, protocol §9). Every operation on a
// handle whose generation no longer matches is a **defined no-op** -- setters do
// nothing, getters return nil, `valid()` returns false -- and never an error.
// A destroyed entity is a routine thing for a script to still be holding, not
// an exceptional one.
//
// But a no-op that is also invisible is a debugging trap, so every one of them
// increments a counter. "My script silently does nothing" has to be
// diagnosable, or the defined-no-op rule just moves the confusion.
//
#include "dnp/sim/world.hpp"
#include "lua/commands.hpp"

struct lua_State;

namespace dnp::server::lua {

using namespace dnp;

struct WorldBindingStats {
    u64 stale_handle_ops = 0;
    u64 commands_queued = 0;
};

// Everything the bindings need, in one place, so the Lua side never reaches
// for a global.
struct WorldContext {
    sim::World*       world = nullptr;
    CommandQueue*     commands = nullptr;
    WorldBindingStats stats{};
};

// Install `dnp` and the entity type. `ctx` must outlive the VM.
void open_world(lua_State* L, WorldContext* ctx);

// Push an entity handle for an agent index, or nil if the slot is empty.
void push_entity(lua_State* L, WorldContext& ctx, u32 index);

}  // namespace dnp::server::lua
