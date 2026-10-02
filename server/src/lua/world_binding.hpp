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
#include <string>
#include <vector>

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

// One HUD value a script set (dnp.hud): kind proto::HUD_INT, HUD_STR or
// HUD_CLEAR.
struct HudSet {
    std::string name;
    u8          kind = 0;
    i32         i = 0;
    std::string s;
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
    // What the player watching `agent` should play when it predicts its own
    // actions: the group `group` (a name hash) replaced by `list`.
    virtual void predict_sounds(u32 level, u32 agent, u32 group,
                                const proto::PredictSound* list, u32 n) = 0;
    // HUD values for whoever watches `agent`, or everyone in the level when it
    // is negative. False when a session would hold more than HUD_MAX_NAMES.
    virtual bool hud(u32 level, i32 agent, const std::vector<HudSet>& set) = 0;
};

struct WorldContext;

// A sound a script asked for this tick (dnp.sound), waiting for the snapshots:
// proto::SoundEvent before it is sent. In the WORLD's coordinates.
struct PendingSound {
    u32 sound  = 0;   // proto::sound_hash of the name
    fx3 pos{};
    u32 source = 0;   // handle raw, 0 for none
    u8  flags  = 0;   // proto::SOUND_*
};
// How many one world keeps between snapshots. Past it a sound is dropped and
// counted, never silently.
inline constexpr u32 MAX_PENDING_SOUNDS = 128;

// THE GAME'S SCRIPTS AND THE LEVELS' SCRIPTS, CALLING EACH OTHER.
//
// A module may declare scripts that run once for the whole game rather than
// once per level (docs/MODULES.md, "Game scripts"): weapons, inventory, a
// HUD -- state that belongs to the player and must not start again in every
// level they walk into. They run in a VM of their own, the GAME VM, which the
// server binds to whichever world the player it is dealing with is in.
//
// They cannot share Lua values with a level's VM, so they call each other by
// NAME, through what each side exported with dnp.export(name, fn): the game
// asks the level the player is in to fire a shot through its monster code
// (dnp.call_level), and a level asks the game what the weapon in a player's
// hands costs (dnp.call_game). Arguments and results are copied: plain data,
// fx, vec3, and entity and prop handles -- the two VMs are looking at the same
// world while the call runs, so a handle means the same thing on both sides.
//
// Not positions in different frames: a level of the region sees its own local
// coordinates and the game VM sees the world's, so what crosses is offsets and
// handles, and each side reads positions for itself.
struct CrossSink {
    virtual ~CrossSink() = default;
    // The game VM, bound to the caller's world, or null when there is none.
    virtual lua_State* enter_game(const WorldContext& caller) = 0;
    // The VM of the level the player body `agent` of the caller's world is in.
    virtual lua_State* enter_level_of(const WorldContext& caller, u32 agent) = 0;
    // The VM of level `level`, which shares the caller's world (a region's
    // neighbour), or null.
    virtual lua_State* enter_level(const WorldContext& caller, u32 level) = 0;
    // After either, once the call is over.
    virtual void leave(lua_State* target) = 0;
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
    // THE LEVEL'S FRAME, when its world holds other levels too (a region).
    //
    // A level's script is written in its own map's terms: Doom-unit positions
    // measured from its own origin, sectors and sides numbered from its own
    // first. In a region the world holds every level on the grid, each copied
    // in at its cell's origin (geom::MapStorage::add_chunk), so every binding
    // that passes a position, a sector number or a side number in or out goes
    // through the helpers below. `chunk` null means the world is this level's
    // alone, and they are all the identity.
    //
    // Sector and SIDE OBJECTS hold the world's index: only the number a script
    // can see is translated. So a sector reached through a neighbour link in
    // another level is still a sector -- its number is just not one this
    // script can name (-1).
    const geom::Chunk* chunk = nullptr;
    fx2               origin{};
    // Which level's script spawned each entity (see Host::owner in main.cpp).
    u32*              owner = nullptr;

    fx3 to_world(fx3 p) const noexcept {
        return chunk ? fx3{p.x + origin.x, p.y + origin.y, p.z} : p;
    }
    fx3 to_local(fx3 p) const noexcept {
        return chunk ? fx3{p.x - origin.x, p.y - origin.y, p.z} : p;
    }
    // The number a script uses for a world sector, or -1 if it is another level's.
    i32 sector_to_local(i32 s) const noexcept {
        if (!chunk || s < 0) return s;
        const u32 u = static_cast<u32>(s);
        if (u < chunk->sectors.first || u >= chunk->sectors.first + chunk->sectors.count) return -1;
        return static_cast<i32>(u - chunk->sectors.first);
    }
    // The world sector a script's number names, or -1 if it names none.
    i32 sector_from_local(i32 s) const noexcept {
        if (!chunk || s < 0) return s;
        if (static_cast<u32>(s) >= chunk->sectors.count) return -1;
        return static_cast<i32>(chunk->sectors.first + static_cast<u32>(s));
    }
    i32 side_to_local(i32 s) const noexcept {
        if (!chunk || s < 0) return s;
        const u32 u = static_cast<u32>(s);
        if (u < chunk->sides.first || u >= chunk->sides.first + chunk->sides.count) return -1;
        return static_cast<i32>(u - chunk->sides.first);
    }
    i32 side_from_local(i32 s) const noexcept {
        if (!chunk || s < 0) return s;
        if (static_cast<u32>(s) >= chunk->sides.count) return -1;
        return static_cast<i32>(chunk->sides.first + static_cast<u32>(s));
    }
    // Record that this level spawned `entity`.
    void own(u32 entity) const noexcept {
        if (owner && entity < sim::MAX_ENTITIES) owner[entity] = level;
    }

    // THE GAME VM's context rather than a level's (see CrossSink). Bound by
    // the server, per call, to the world of the player concerned, in that
    // world's own coordinates -- `chunk` stays null.
    bool              game = false;
    // The module's control scheme (module.txt `controls`): 0 platformer, 1
    // fps. What dnp.controls() answers, so a weapon aims by the view in an FPS
    // and by auto-aim in a platformer.
    u8                controls = 0;
    // Which player each body is, as a number that stays the same from level to
    // level (a session's id), or 0 for a body nobody plays. Borrowed from the
    // host. e:player() reads it; null means nobody is anybody (a test).
    const u32*        player = nullptr;
    CrossSink*        cross = nullptr;
    // The world's sounds waiting for the snapshots (dnp.sound). Borrowed from
    // the host; null means a script's sounds go nowhere (a test).
    std::vector<PendingSound>* sounds = nullptr;
    u32               sounds_dropped = 0;

    // Whether the level is being RESTORED from saved state (docs/SAVING.md):
    // a script's own placement of monsters and props stands aside, because the
    // libraries' load hooks put back what was there instead.
    bool              restoring = false;
    // Spawns refused because the level was being restored (see restoring).
    u32               held_back_spawns = 0;

    i32*              hp = nullptr;
    // Which avatar draws each player (design 4.1). Borrowed like hp, and server
    // state for the same reason: a costume is not physics, so it is not hashed,
    // not in the determinism gate and not predicted.
    u16*              avatar = nullptr;
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

// --- persistence (docs/SAVING.md) ---------------------------------------
//
// A script library saves its own state through dnp.persist(name, save, load):
// `save()` returns a plain value -- tables, numbers, strings, booleans, fx,
// vec3 -- and `load(value)` puts it back. The engine stores what save returned
// with the level and hands it to load when the level is restored. Players the
// same way through dnp.persist_player(name, save(e), load(e, value)).
//
// The encoding is text, deterministic (keys sorted), and refuses anything
// that is not plain data: a function or a handle saved today would mean
// nothing when it is read back.

// Encode the value at `idx`. False, with `err` saying where, on anything that
// cannot be saved.
bool encode_value(lua_State* L, int idx, std::string& out, std::string& err);
// Decode one value and push it. False (nothing pushed) on malformed input.
bool decode_value(lua_State* L, const std::string& in, std::string& err);

// Every level part registered with dnp.persist, saved: { [name] = save() }.
// A part whose save fails is left out and named in `err`; the rest are kept.
bool persist_capture(lua_State* L, std::string& out, std::string& err);
// Hand each saved part to its load. Parts with no loader are named in `err`.
bool persist_restore(lua_State* L, const std::string& in, std::string& err);
// The same for one player.
bool persist_player_capture(lua_State* L, WorldContext& ctx, u32 agent,
                            std::string& out, std::string& err);
bool persist_player_restore(lua_State* L, WorldContext& ctx, u32 agent,
                            const std::string& in, std::string& err);

// Push an entity handle for an agent index, or nil if the slot is empty.
void push_entity(lua_State* L, WorldContext& ctx, u32 index);

// For the binding files beside this one: the context this VM is attached to
// (raises when none is), and the agent an entity argument names NOW -- -1 for
// a stale handle, the one rule every verb here follows.
WorldContext& context_of(lua_State* L);
i32 entity_agent(lua_State* L, int idx);

// The weapon machine's bindings (weapon_binding.cpp), onto the `dnp` table at
// the top of the stack.
void open_weapons(lua_State* L);

// The weapon table a VM's weapons.lua compiled, and the seed its random stream
// uses (which is the table's hash, so a client holding it knows both); null
// when that VM has compiled none.
const sim::WeaponTable* compiled_weapons(lua_State* L, u64* seed);

// Push a prop handle -- the same entity userdata an agent uses, because from a
// script's point of view a key and a player are both things with a handle.
void push_prop(lua_State* L, WorldContext& ctx, u32 entity);

// Push a sector handle. Unchecked: sector userdata carries no generation and
// every method range-checks, so an out-of-range index is a nil-returning
// handle rather than a fault.
void push_sector(lua_State* L, u32 index);

}  // namespace dnp::server::lua
