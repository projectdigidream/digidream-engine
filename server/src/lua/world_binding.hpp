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
#include "dnp/proto/messages.hpp"
#include "dnp/geom/mapstore.hpp"
#include "dnp/math/rng.hpp"
#include "dnp/sim/world.hpp"
#include "dnp/nav/navgraph.hpp"
#include "lua/commands.hpp"
#include "lua/effects.hpp"

struct lua_State;

namespace dnp::server::lua {

using namespace dnp;

struct WorldBindingStats {
    u64 stale_handle_ops = 0;
    u64 commands_queued = 0;
};

// Everything the bindings need, in one place, so the Lua side never reaches
// for a global.
// Where a script's request to move a player to another map goes.
//
// An interface rather than a direct call into the server, for the reason every
// mutation in this file is deferred: a map change despawns an entity from one
// world and spawns it in another, and doing that from inside a callback would
// mutate the entity set a tick is iterating. The host queues it and applies it
// at a defined point.
// Where a script's request for a prop KIND by name is answered.
//
// The catalogue is a text file the operator ships (assets/props.txt), so the
// binding layer cannot know the names at compile time and must not try -- the
// whole point is that adding a prop is editing a file, not rebuilding anything.
struct PropCatalogueView {
    virtual ~PropCatalogueView() = default;
    // Returns 0 when there is no such prop, which is a script naming something
    // the operator did not install.
    virtual u16 kind_by_name(const char* name, size_t len) const = 0;
    // The simulation's half of a prop's definition. A kind with no entry gets
    // the engine's defaults rather than a zero-sized pickup nothing can touch.
    virtual fx  radius_of(u16 kind) const = 0;
    virtual fx  height_of(u16 kind) const = 0;
    // Nothing installed at all, which is a different failure from a name that
    // is merely absent and deserves a different message.
    virtual bool empty() const = 0;
    virtual u8   flags_of(u16 kind) const = 0;
    // World units per second, or zero for "the engine's default".
    virtual fx   speed_of(u16 kind) const = 0;
    // What a script calls it. Behaviour is written per NAME, because a number
    // in a script is a number somebody has to look up in a file the script does
    // not mention.
    virtual const char* name_of(u16 kind) const = 0;
};

struct MapChangeSink {
    virtual ~MapChangeSink() = default;
    // `to` is a level index, or negative for "the next one".
    virtual void request_map_change(u32 level, u32 agent, i32 to) = 0;
};

// Where a panel goes. Implemented by the server, which owns the sessions.
//
// A UI belongs to a PERSON, not to a world: the simulation has agents and knows
// nothing about who is watching one. So this is a sink like MapChangeSink
// rather than something reachable through sim::World, and a script addresses a
// panel by entity because that is the only handle on a person it has.
// What levels this server was started with, by name.
//
// A script that offers a map selector must show what the OPERATOR loaded, not a
// list written into the script -- which would go stale the first time somebody
// ran the server with different arguments, and would offer maps that are not
// there.
struct LevelListSink {
    virtual ~LevelListSink() = default;
    virtual u32         level_count() const = 0;
    virtual const char* level_name(u32 i) const = 0;
};

struct UiSink {
    virtual ~UiSink() = default;
    // Returns the allocated ui_id, or 0 if the agent has nobody watching it --
    // which is the ordinary case for a monster, and is not an error.
    virtual u32  open_ui(u32 level, u32 agent, const proto::UiPanel& panel) = 0;
    virtual void close_ui(u32 level, u32 agent, u32 ui_id) = 0;
    // One widget's value, in a panel already open. A no-op for a panel that is
    // not, or a widget it does not contain.
    virtual void set_ui(u32 level, u32 agent, u32 ui_id, u16 widget_id, i32 value) = 0;
    // Camera settings for whoever is watching this agent (design §4.1: the
    // camera is the client's, and Lua configures and triggers it).
    virtual void set_camera(u32 level, u32 agent, const proto::CameraSet& c) = 0;
    // The level's camera defaults: sent to everyone who joins it, and at once
    // to whoever is already there. What a map's Lua says with dnp.level_camera.
    virtual void set_level_camera(u32 level, const proto::CameraSet& c) = 0;
    // A side's picture changes -- a switch thrown. Broadcast to everyone in the
    // level and remembered, because a switch thrown before you arrived is part
    // of the world you are arriving in.
    virtual void set_side_texture(u32 level, const proto::SideTexture& t) = 0;
    // The map's music changes cue, for whoever is watching `agent` -- or for
    // everyone in the level when it is negative. Not remembered: a cue is a
    // moment, and a player joining later starts on the map's default cue.
    virtual void music(u32 level, i32 agent, const proto::MusicCue& m) = 0;
    // The curtain and the letterbox bars, for whoever is watching `agent` -- or
    // for everyone in the level when it is negative. Like music, not
    // remembered: a fade is a moment in a cutscene, and a player joining
    // halfway through one should arrive at a screen they can see out of.
    virtual void screen(u32 level, i32 agent, const proto::ScreenSet& s) = 0;
};

struct WorldContext {
    sim::World*       world = nullptr;
    geom::MapStorage* map_store = nullptr;

    // The level's sector graph, for dnp.find_path. Borrowed, not owned: the
    // server builds one per level at load and rebuilds it when a shape commits,
    // and a second copy here would path over a map that no longer exists.
    const nav::NavGraph* nav = nullptr;

    // Which level this context belongs to, and where to send a map change. The
    // level index is fixed at load; a script never names its own level, it just
    // says "next".
    u32               level = 0;

    // dnp.find_path's per-tick budget, PER LEVEL.
    //
    // These were function-static in world_binding.cpp, which is one budget for
    // every level in the process, and it failed in both directions at once.
    // Levels advance their own tick counters: two levels on the same tick SHARE
    // the sixty-four paths, so a busy level starves a quiet one; two levels on
    // different ticks each see `now != budget_tick`, reset the counter, and the
    // budget never applies to either. A server hosting thirty-two Doom levels
    // had both happening depending on join order.
    //
    // Here instead, because a WorldContext is what a level has one of. Not in
    // sim::World: a pathfinding budget is a property of the script host, and
    // core/ has no idea Lua exists.
    u32               path_budget_tick = 0xFFFFFFFFu;
    u32               path_budget_used = 0;

    // Ask for a path, against that budget. Returns false when the budget is
    // spent, which is a refusal and not "no route" -- the two are different
    // facts and a caller that conflates them reports a shut door.
    //
    // HERE rather than inside dnp.find_path, because the C++ mob loop asks for
    // paths too and was going straight to nav::find_path. One budget with two
    // spenders and only one of them counting is a behaviour difference that
    // appears only at scale: with a handful of monsters nothing is ever
    // refused, and with a level of them the Lua runtime starts giving up while
    // the C++ one does not.
    static constexpr u32 PATHS_PER_TICK = 64;
    bool take_path_budget(u32 now) {
        if (now != path_budget_tick) { path_budget_tick = now; path_budget_used = 0; }
        if (path_budget_used >= PATHS_PER_TICK) return false;
        ++path_budget_used;
        return true;
    }
    MapChangeSink*    map_changes = nullptr;
    UiSink*           ui = nullptr;
    LevelListSink*    levels = nullptr;

    // Health, one per agent slot.
    //
    // SERVER state, not simulation state. The core has no notion of health --
    // it reports that a swing reached something or that a ceiling closed on
    // somebody, and what that costs is decided elsewhere. Keeping it here means
    // it is not hashed, not in the determinism gate, and not something a client
    // predicts; it reaches the right client through the snapshot's private
    // block, which design section 2 already requires be per-observer.
    i32*              hp = nullptr;
    const PropCatalogueView* props = nullptr;

    // Deterministic, seeded from the map hash and the script name -- never from
    // a clock or /dev/urandom. A script that behaved differently on replay
    // would destroy the debuggability the command log buys, and "random" in a
    // simulation means "reproducibly arbitrary", not "unpredictable to us".
    Rng               rng{1, 1};
    CommandQueue*     commands = nullptr;
    EffectStack*      effects = nullptr;

    // Set whenever a script changes the effect set. The host recomputes the
    // affected agents' parameters from base and replicates the difference --
    // never incrementally, see effects.hpp.
    bool              effects_dirty = false;

    WorldBindingStats stats{};

    fx prop_radius(u16 kind) const {
        return props ? props->radius_of(kind) : fx::from_frac(1, 2);
    }
    fx prop_height(u16 kind) const {
        return props ? props->height_of(kind) : fx::from_int(1);
    }
    u8 prop_flags(u16 kind) const { return props ? props->flags_of(kind) : 0; }
};

// Install `dnp` and the entity type. `ctx` must outlive the VM.
void open_world(lua_State* L, WorldContext* ctx);

// Push an entity handle for an agent index, or nil if the slot is empty.
void push_entity(lua_State* L, WorldContext& ctx, u32 index);

// Push a prop handle -- the same entity userdata an agent uses, because from a
// script's point of view a key and a player are both things with a handle.
void push_prop(lua_State* L, WorldContext& ctx, u32 entity);

// Push a sector handle. Unchecked: sector userdata carries no generation and
// every method range-checks, so an out-of-range index is a nil-returning
// handle rather than a fault.
void push_sector(lua_State* L, u32 index);

}  // namespace dnp::server::lua
