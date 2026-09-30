//
// Authoritative server: fixed-timestep loop, link endpoint, snapshot production.
//
// Phase 1 harness. The simulation is the placeholder point-mass controller from
// core/sim/world.cpp, and there is no Lua and no map yet. What IS real is the
// shape: the tick loop, the input jitter buffer, per-observer snapshot
// encoding, delta compression against an acked baseline, and budget
// enforcement.
//
#include <chrono>
#include <memory>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>
#include <string>
#include <algorithm>
#include <dirent.h>

#include "dnp/proto/messages.hpp"
#include "dnp/geom/mapfile.hpp"
#include "dnp/math/rng.hpp"
#include "dnp/nav/navgraph.hpp"
#include "dnp/physics/params_table.hpp"
#include "dnp/sim/testmap.hpp"
#include "dnp/sim/world.hpp"
#include "lua/commands.hpp"
#include "lua/effects.hpp"
#include "lua/vm.hpp"
#include "props.hpp"
#include "lua/world_binding.hpp"
#include "world/metamap.hpp"

// For the event dispatch below, which pushes plain Lua values (a side index, a
// crush kind) alongside the userdata the binding layer builds.
extern "C" {
#include "lua.h"
}
#include "net/link.hpp"
#include "session.hpp"
#include "snapshot_history.hpp"

using namespace dnp;
using namespace dnp::server;

namespace {

// Wall clock lives HERE, in server/, and never in core/. The simulation's notion
// of time is a tick counter; this only decides when the next tick should happen
// in real time, and its jitter has no effect on what the tick computes.
using Clock = std::chrono::steady_clock;
constexpr auto TICK_PERIOD = std::chrono::nanoseconds(1'000'000'000 / sim::TICK_RATE_HZ);

// Chunk size for MAP_DATA. Well under the link frame cap, and small enough that
// a slow client does not stall behind one enormous write.
//
// Settable (--map-chunk) for TESTS ONLY: the seam fixture's maps are under a
// kilobyte, and a fault that needs a map to span several chunks -- "chunk out
// of order (expected 0, got 65536)" was one -- does not reproduce in one.
static size_t MAP_CHUNK_BYTES = 8192;

// One map, and everything that exists per map.
//
// A LEVEL, not a server. The request that shaped this is "move that client to
// the next map, and only that client", which means two players can be standing
// in different maps at the same moment -- and a simulation is defined against
// one set of geometry, so there has to be one simulation per map. Everything
// below was a Server member until that was true: the world, the map, the
// scripts, and every piece of per-agent bookkeeping that is indexed by an agent
// slot, because slot 3 in MAP01 and slot 3 in MAP02 are different players.
//
// The consequence is worth stating rather than discovering: players in
// different levels cannot see each other, cannot collide, and appear in none of
// each other's snapshots. That is what "a different map" means.
//
// Heap-allocated by the server (roughly 700 KB each), so hosting the whole of
// DOOM2 is about twenty megabytes and the count is not a thing to ration.
// How often the nav graph may be rebuilt while sectors are moving. 15 ticks is
// a quarter of a second: faster than a monster can walk to a door it has just
// noticed, and a twentieth of the rebuilds that doing it every tick would cost.
inline constexpr u32 NAV_REBUILD_TICKS = 15;

// WHERE A LEVEL'S BODIES ARE SIMULATED, and everything kept per body.
//
// A level used to own its World outright. Now a World can hold several levels
// at once (geom::MapStorage::add_chunk), so what is per body -- health, avatar,
// what each client has been told about motion and props -- belongs to the
// World the body is in, not to whichever level's script happens to own it.
//
// A level off the metamap's grid has a Host of its own, reading that level's
// map directly, exactly as before. Levels on the grid share one (the region).
inline constexpr u32 NO_OWNER = 0xFFFFFFFFu;

struct Host {
    sim::World       world;
    // The geometry `world` collides against: the owning level's own store for
    // a host of its own, or the region's store of chunks.
    geom::MapStorage* store = nullptr;
    geom::MapData*    view = nullptr;

    // A REGION owns its store: every level on the grid, each copied in at its
    // cell's origin (geom::MapStorage::add_chunk). Null for a level's own host,
    // which reads that level's map.
    std::unique_ptr<geom::MapStorage> region_store;
    geom::MapData                     region_view{};
    bool is_region() const noexcept { return region_store != nullptr; }

    // WHICH LEVEL'S SCRIPT SPAWNED EACH ENTITY, or NO_OWNER. In a region every
    // level's monsters and props share one world; this is how an event about
    // one reaches the script that is keeping track of it, and how a level
    // being unloaded knows what to take with it. Unused by a host of its own.
    u32  owner[sim::MAX_ENTITIES];

    Host() { for (u32 i = 0; i < sim::MAX_ENTITIES; ++i) owner[i] = NO_OWNER; }

    // Health per agent slot. Server state: see WorldContext::hp.
    i32              hp[sim::MAX_AGENTS];

    // The tick at which a script last changed each agent. Compared against the
    // tick of the last snapshot sent to a session, so LUA_APPLIED is set for
    // the observers a change actually concerns and nobody else -- setting it
    // globally would excuse every client's mismatch on every scripted tick.
    u32  lua_touched_tick[sim::MAX_AGENTS]{};

    u32  crush_reported_tick[sim::MAX_AGENTS]{};
    bool crush_seen[sim::MAX_AGENTS]{};
    u8   param_epoch[sim::MAX_AGENTS]{};
    // Which avatar draws each player (design 4.1). SERVER state, not simulation
    // state -- like hp: not hashed, not in the determinism gate, not predicted.
    // A costume is not physics.
    u16  avatar[sim::MAX_AGENTS]{};

    // Parameter effects, per agent: they are computed from an agent's base, and
    // an agent is in exactly one world.
    lua::EffectStack   effects;

    // Navigation over the whole of `view`, in this world's sector indices. For
    // a region that is every resident level at once, so a monster can path
    // from one level into the next like between two rooms.
    nav::NavGraph    nav_graph;
    // THE NAV GRAPH GOES STALE THE MOMENT A DOOR MOVES (see refresh_nav_if_stale):
    // a fingerprint over the moving sectors' heights, and the tick of the last
    // rebuild, which throttles it.
    u64              nav_heights = 0;
    u32              nav_built_tick = 0;

    // The motion descriptors every client of this world has been told about.
    sim::SectorMotion sent_motions[sim::MAX_MOVING_SECTORS]{};
    u32 sent_motion_count = 0;
    u32 motion_changed_tick = 0;

    // The same, for the set of props. A barrel exploding under someone is a
    // scripted change they could not have predicted, and without the label it
    // reads as a determinism fault every time one goes off.
    u32 prop_epoch_seen = 0;
    u32 props_changed_tick = 0;

    // Sectors whose motion is scripted-discrete, collected once per tick so the
    // snapshot encoder does not rescan.
    proto::SectorOffset discrete_offsets[proto::MAX_SECTOR_OFFSETS];
    u16                 discrete_count = 0;
};

struct Level {
    // A Host of its own unless given one to share. The references below are
    // bound to whichever it is, so everything written against a level's world
    // and its per-body arrays reads exactly as it did when the level owned
    // them.
    explicit Level(Host* shared = nullptr)
        : own(shared ? nullptr : std::make_unique<Host>()),
          host(shared ? shared : own.get()),
          hp(host->hp), world(host->world),
          effects(host->effects),
          nav_graph(host->nav_graph), nav_heights(host->nav_heights),
          nav_built_tick(host->nav_built_tick),
          lua_touched_tick(host->lua_touched_tick),
          crush_reported_tick(host->crush_reported_tick), crush_seen(host->crush_seen),
          param_epoch(host->param_epoch), avatar(host->avatar),
          sent_motions(host->sent_motions), sent_motion_count(host->sent_motion_count),
          motion_changed_tick(host->motion_changed_tick),
          prop_epoch_seen(host->prop_epoch_seen), props_changed_tick(host->props_changed_tick),
          discrete_offsets(host->discrete_offsets), discrete_count(host->discrete_count) {
        if (own) { own->store = &map_store; own->view = &view; }
    }
    Level(const Level&) = delete;
    Level& operator=(const Level&) = delete;

    std::unique_ptr<Host> own;
    Host*                 host;

    // Side textures a script has changed, in the order it changed them.
    //
    // Remembered rather than merely broadcast: a client joining later must see
    // the switches that have already been thrown, and one that had to witness
    // the event to know the state would show every returning player a different
    // wall. Same reasoning as the sector motions re-sent on join.
    std::vector<proto::SideTexture> side_textures;
    // The level's camera defaults (dnp.level_camera), sent to everyone who
    // joins. Not simulation state; one CAMERA_SET per joining client.
    proto::CameraSet camera;
    bool             has_camera = false;

    // THE LEVEL'S OWN MAP: its bytes and its parse. Local coordinates, whatever
    // host simulates it. (Navigation is the host's: see Host::nav_graph.)
    geom::MapStorage map_store;
    std::vector<u8>  map_bytes;   // exactly what clients receive
    u64              map_hash = 0;

    // The view of the level's own map. A member, not a function-local static:
    // one static was correct while there was exactly one map and would have
    // made every level after the first alias the last one loaded -- a bug that
    // presents as "the map renders right and collides wrong".
    geom::MapData    view{};

    // One VM per level, which is not merely convenient: a converted map's
    // script is written against its own side indices and sector tags, and two
    // of them sharing a global `on_interact` would have the second silently
    // replace the first.
    lua::Vm            vm;
    lua::CommandQueue  commands;
    lua::WorldContext  ctx;
    bool               scripted = false;

    // The host's, by reference (see the constructor).
    i32 (&hp)[sim::MAX_AGENTS];
    sim::World&        world;
    lua::EffectStack&  effects;
    nav::NavGraph&     nav_graph;
    u64&               nav_heights;
    u32&               nav_built_tick;
    u32  (&lua_touched_tick)[sim::MAX_AGENTS];
    u32  (&crush_reported_tick)[sim::MAX_AGENTS];
    bool (&crush_seen)[sim::MAX_AGENTS];
    u8   (&param_epoch)[sim::MAX_AGENTS];
    u16  (&avatar)[sim::MAX_AGENTS];
    sim::SectorMotion (&sent_motions)[sim::MAX_MOVING_SECTORS];
    u32& sent_motion_count;
    u32& motion_changed_tick;
    u32& prop_epoch_seen;
    u32& props_changed_tick;
    proto::SectorOffset (&discrete_offsets)[proto::MAX_SECTOR_OFFSETS];
    u16& discrete_count;
};

struct Server : lua::MapChangeSink, lua::UiSink, lua::LevelListSink {
    net::LinkServer      link;

    // Which named spawn arriving players land on. Portals supply this per
    // player via entry_spawn_tag (design §3.3); until then it is server-wide.
    const char* spawn_tag = "start";
    u32         spawn_tag_len = 5;

    // The levels this server hosts, in the order they were given on the command
    // line -- which IS the progression order. `--map a --map b --map c` means an
    // exit in a takes you to b.
    // A LEVEL THE SERVER KNOWS ABOUT, loaded or not.
    //
    // The split is the whole of dynamic loading. What a level IS -- its path,
    // its name, where the metamap put it -- is small and permanent; what a
    // level HOLDS is four megabytes and exists only while somebody is near it.
    //
    // TOMBSTONED RATHER THAN ERASED, and that is not squeamishness: a session's
    // `level`, a script's `ctx.level`, `entity:change_map(i)` and `dnp.levels()`
    // are all indices into this vector, so erasing one renumbers every level
    // after it and every one of those starts naming a different map. A slot
    // whose payload is null costs a pointer.
    struct LevelSlot {
        std::string path;
        mutable std::string short_name;

        // Where the metamap put it. Off the grid is not an error: an unplaced
        // level is loaded when it is entered and dropped when it is left, with
        // no neighbours. See server/src/world/metamap.hpp.
        server::world::LevelCell cell;

        // Null when the level is not resident.
        std::unique_ptr<Level> live;
    };
    std::vector<LevelSlot> levels;

    // THE TICK EVERY LEVEL COUNTS IN.
    //
    // Not a convenience: levels are on ONE clock and always have been. Every
    // level used to be loaded at startup and stepped from tick zero together,
    // and Server::tick goes out of its way to keep an empty one counting rather
    // than letting it stop -- in a comment that records what happened when it
    // did. A client crossing into a level thousands of ticks behind jumps its
    // own clock backwards far enough that the server's input ring seeds itself
    // from a straggler and discards everything real after it, which was
    // reported as "player inputs stop working after transitioning to a map".
    //
    // An unloaded level cannot count, so a level being loaded is set to this
    // rather than to zero or to whatever it read when it was dropped. A level
    // that has never been loaded and one that was away for an hour arrive at
    // the same answer, which is the right one: the clock counts TICKS OF THIS
    // SERVER, not time spent in a level.
    u32 clock_ = 0;

    // Where the levels sit, if anything said. An empty metamap is the old
    // behaviour exactly: no level has a cell, so no level has a neighbour, and
    // residency is "whoever is being played".
    server::world::MetaMap meta;

    // Scratch for the residency pass, kept so a level change does not allocate.
    std::vector<server::world::LevelCell> meta_cells;
    std::vector<bool> meta_occupied;
    std::vector<bool> meta_want;

    // Scripts loaded into every level, before that level's own. The shared
    // action library lives here; the map's own data file is found beside its
    // .dnpm and loaded per level.
    std::vector<const char*> shared_scripts;

    lua::VmConfig vm_config{};
    bool          scripts_enabled = false;

    // What kinds of prop exist. Server-wide, not per level: a key is a key in
    // every map, and a catalogue per level would mean the same name meaning
    // different things depending on where you were standing.
    PropCatalogue props;

    // UNCHECKED, and it always was. Every caller either holds a session that is
    // in this level -- which cannot be unloaded while they are -- or has just
    // tested is_live. The bounds comment on ingest() below says the same thing
    // about the range.
    Level& level(u32 i) { return *levels[i].live; }

    // Is this level's payload in memory? False for a level that exists, is
    // playable, and nobody is near.
    bool is_live(u32 i) const {
        return i < levels.size() && levels[i].live != nullptr;
    }
    // Also LevelListSink's, so a script can ask what maps this server loaded.
    u32 level_count() const override { return static_cast<u32>(levels.size()); }

    void collect_discrete_offsets(Level& L) {
        L.discrete_count = 0;
        const auto& ms = L.world.motions();
        for (u32 i = 0; i < ms.count() && L.discrete_count < proto::MAX_SECTOR_OFFSETS; ++i) {
            const sim::SectorMotion& m = ms.at(i);
            // Parametric movers are deliberately absent: the client computes
            // theirs from the parameters it already holds, and sending it too
            // would create a second source of truth that could disagree.
            if (m.type != sim::MotionType::ScriptedDiscrete) continue;
            // By NAME: the observer's store may hold a different set of maps.
            L.discrete_offsets[L.discrete_count].sector =
                static_cast<u32>(L.world.sector_name(static_cast<i32>(m.sector)));
            L.discrete_offsets[L.discrete_count].offset = m.offset;
            ++L.discrete_count;
        }
    }

    void send_motion(const Level& L, u32 id, const sim::SectorMotion& m) {
        u8 buf[128];
        ByteWriter w(buf, sizeof(buf));
        const size_t len_pos = w.mark();
        w.u32v(0);
        w.u8v(proto::MSG_SECTOR_MOTION_SET);
        // The sector's NAME, not this store's index for it (map.hpp, sector
        // names): the client resolves it against whatever it holds.
        w.u32v(static_cast<u32>(L.world.sector_name(static_cast<i32>(m.sector))));
        w.u8v(static_cast<u8>(m.type));
        w.u8v(static_cast<u8>(m.target));
        w.u32v(m.start_tick);
        w.u32v(m.duration_ticks);
        w.u8v(static_cast<u8>(m.loop));
        w.vec3_raw(m.from);
        w.vec3_raw(m.to);
        // The turn, appended: where it pivots and how far round it goes. The
        // endpoints are SIGNED and UNWRAPPED brad, which is why they are 64 bit
        // -- a u32 cannot say "twice round" or "the other way".
        w.fxraw(m.pivot.x);
        w.fxraw(m.pivot.y);
        w.u64v(static_cast<u64>(m.spin_from));
        w.u64v(static_cast<u64>(m.spin_to));
        const size_t end = w.size();
        const u32 body = static_cast<u32>(end - len_pos - 4);
        buf[len_pos]     = static_cast<u8>(body & 0xFF);
        buf[len_pos + 1] = static_cast<u8>((body >> 8) & 0xFF);
        buf[len_pos + 2] = static_cast<u8>((body >> 16) & 0xFF);
        buf[len_pos + 3] = static_cast<u8>((body >> 24) & 0xFF);
        link.send_control(id, buf, end);
    }

    void send_motion_sets(Level& L, u32 id) {
        const auto& ms = L.world.motions();
        for (u32 i = 0; i < ms.count(); ++i) send_motion(L, id, ms.at(i));
    }

    // Two parametric motions are the same motion iff a client computing either
    // one would land on the same offset for every tick. That is exactly the
    // descriptor -- endpoints, start, duration, loop, target, and the turn --
    // and NOT the current offset or angle, which are derived from it.
    static bool same_motion(const sim::SectorMotion& a, const sim::SectorMotion& b) {
        return a.sector == b.sector && a.type == b.type && a.target == b.target &&
               a.start_tick == b.start_tick && a.duration_ticks == b.duration_ticks &&
               a.loop == b.loop && a.from == b.from && a.to == b.to &&
               a.pivot == b.pivot && a.spin_from == b.spin_from &&
               a.spin_to == b.spin_to;
    }

    // A door opened mid-match has to reach the clients, or they predict against
    // a sector that never moved.
    //
    // Sent on CHANGE rather than every tick, and on the reliable channel: a
    // parametric motion is a closed form, so one message buys every tick of it
    // forever (§3.1.2) -- but only if the message arrives. Losing it would leave
    // a client walking into a door that is open on the server, which no later
    // snapshot repairs, because snapshots carry entities and not geometry.
    void replicate_motion_changes(u32 li) {
        Level& L = level(li);
        const auto& ms = L.world.motions();
        for (u32 i = 0; i < ms.count(); ++i) {
            const sim::SectorMotion& m = ms.at(i);
            if (i < L.sent_motion_count && same_motion(L.sent_motions[i], m)) continue;
            L.sent_motions[i] = m;
            L.motion_changed_tick = L.world.tick() + 1;
            // Only the sessions IN this level. A door opening in MAP01 is not
            // news to somebody standing in MAP02, and sending it would name a
            // sector index that means something else entirely there.
            for (auto& [id, s] : sessions) {
                if (s.hello_done && same_world(s, li)) send_motion(L, id, m);
            }
        }
        if (ms.count() > L.sent_motion_count) L.sent_motion_count = ms.count();
    }

    // --- movement parameters (docs/protocol.md 8.4) --------------------------
    //
    // The parameter table is the RULES the client predicts with, not state it
    // can be corrected toward. A client running different parameters mispredicts
    // every tick and never recovers, so these go on the reliable channel and
    // carry a generation the snapshot echoes back.



    // Send one agent's parameters. `full` sends every field, which is what a
    // joining client needs because it has no history to patch.
    void send_param_update(Level& L, u32 id, u32 agent, bool full,
                           const u16* ids = nullptr, u16 id_count = 0) {
        proto::ParamUpdate p;
        p.handle         = proto::Handle::make(agent + 1, L.world.generation(agent));
        p.epoch          = L.param_epoch[agent];
        p.effective_tick = L.world.tick();
        p.flags          = full ? proto::param_flag::FULL : 0;

        const physics::MoveParams& mp = L.world.params(agent);
        if (full) {
            p.count = physics::PARAM_COUNT;
            for (u16 i = 0; i < physics::PARAM_COUNT; ++i) {
                p.fields[i].id  = i;
                p.fields[i].raw = physics::param_get_raw(mp, i);
            }
        } else {
            p.count = id_count > proto::MAX_PARAM_FIELDS
                          ? static_cast<u16>(proto::MAX_PARAM_FIELDS) : id_count;
            for (u16 i = 0; i < p.count; ++i) {
                p.fields[i].id  = ids[i];
                p.fields[i].raw = physics::param_get_raw(mp, ids[i]);
            }
        }

        u8 buf[8 + 16 + proto::MAX_PARAM_FIELDS * 10];
        ByteWriter w(buf, sizeof(buf));
        const size_t len_pos = w.mark();
        w.u32v(0);
        w.u8v(proto::MSG_PARAM_UPDATE);
        proto::encode_param_update(w, p);
        const size_t end = w.size();
        if (!w.ok()) {
            std::fprintf(stderr, "[srv] param update for agent %u did not fit\n", agent);
            return;
        }
        const u32 body = static_cast<u32>(end - len_pos - 4);
        buf[len_pos]     = static_cast<u8>(body & 0xFF);
        buf[len_pos + 1] = static_cast<u8>((body >> 8) & 0xFF);
        buf[len_pos + 2] = static_cast<u8>((body >> 16) & 0xFF);
        buf[len_pos + 3] = static_cast<u8>((body >> 24) & 0xFF);
        link.send_control(id, buf, end);
    }

    // Change one parameter and tell everyone. This is the shape the Lua binding
    // will take in slice 5: mutate, bump the epoch, broadcast.
    //
    // Broadcast to EVERY session, not just the owner. Remote clients do not
    // predict other players (§11 interpolates them), but they will once
    // entities carry parameters, and a rule that is "reliable for one observer
    // and absent for the rest" is the kind of asymmetry that is discovered
    // years later.
    void set_param(u32 li, u32 agent, u16 field_id, i64 raw) {
        Level& L = level(li);
        if (agent >= sim::MAX_AGENTS) return;
        if (!physics::param_set_raw(L.world.params_mut(agent), field_id, raw)) return;
        // Wraps deliberately; only equality is ever tested (docs/protocol.md
        // 10.1.2), and 255 intervening changes cannot happen inside one round
        // trip.
        L.param_epoch[agent] = static_cast<u8>(L.param_epoch[agent] + 1);
        for (auto& [id, s] : sessions) {
            if (s.hello_done && same_world(s, li)) send_param_update(L, id, agent, false, &field_id, 1);
        }
    }

    // --- where a tick goes -----------------------------------------------------
    //
    // WHY THIS EXISTS AT ALL. "Some ticks are taking way too long" was reported
    // against a number measured on an Android emulator, and the native server
    // was answering `0 stalls` on the same level. There was no way to ask where
    // the time went without patching `stall_warn_us` and rebuilding, which is
    // what had to be done by hand to find out.
    //
    // Three phases, because they are the three things that scale differently
    // with the number of monsters: Lua decides, physics resolves, and the
    // snapshot encodes. A total alone says a tick was late; it does not say
    // which of the three to look at, and guessing has cost this repository time
    // before -- see the stall detector's own comment in lua/vm.cpp.
    //
    // OFF BY DEFAULT and gated on one branch. `now_us()` is a syscall on some
    // platforms and this runs per tick per level; a measurement that changes
    // what it measures is worse than none.
    // Ticks between hash-trace lines; 0 disables it. See the trace itself.
    u32 hash_trace = 0;

    struct Perf {
        u32 every = 0;          // ticks per report; 0 disables the whole thing
        u32 ticks = 0;
        u64 lua_us = 0;
        u64 step_us = 0;
        u64 snap_us = 0;
        u64 worst_total_us = 0;
        // Bodies that fell out of the agent broadphase and back onto the full
        // O(n^2) sweep, summed over the whole reporting window. Summed rather
        // than sampled at report time: the counters on World describe the LAST
        // tick, and a report every 120 ticks that reads the last one sees 1 in
        // 120 of them. A fallback that happens in bursts -- which is exactly
        // how it would happen, since it is driven by bodies moving fast -- would
        // then be invisible almost every time.
        u64 bp_full = 0;
    };
    Perf perf{};

    static u64 now_us() {
        using namespace std::chrono;
        return static_cast<u64>(
            duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
    }

    // Called once per tick, after send_snapshots, with the snapshot cost.
    void perf_report(u64 snap_us) {
        if (perf.every == 0) return;
        perf.snap_us += snap_us;
        const u64 total = perf.lua_us + perf.step_us + perf.snap_us;
        if (++perf.ticks < perf.every) return;

        // Agents, not entity slots: the question is how many BODIES the tick
        // resolved, and MAX_AGENTS says nothing about that.
        u32 agents = 0;
        u32 resident = 0;
        for (u32 li = 0; li < level_count(); ++li) {
            if (!is_live(li)) continue;
            ++resident;
            for (u32 i = 0; i < sim::MAX_AGENTS; ++i) {
                if (level(li).world.agent(i).active) ++agents;
            }
        }
        const double n = static_cast<double>(perf.ticks);
        std::fprintf(stderr,
                     "[perf] %u ticks  agents %u  levels %u/%u  lua %.0f us  step %.0f us"
                     "  snap %.0f us  total %.0f us of %u  worst %llu us%s\n",
                     perf.ticks, agents, resident, level_count(),
                     perf.lua_us / n, perf.step_us / n, perf.snap_us / n,
                     total / n, 1000000u / sim::TICK_RATE_HZ,
                     static_cast<unsigned long long>(perf.worst_total_us),
                     perf.bp_full ? "  [broadphase fell back]" : "");
        // The broadphase falls back to the full O(n^2) sweep for any body whose
        // query it cannot bound. The answer is the same either way -- the
        // fallback IS the reference sweep -- but the cost is the cost it was
        // built to avoid, so a level quietly running every body down it would
        // look exactly like a broadphase that simply does not help. Said out
        // loud, with a rate, so it is a number rather than a mystery.
        if (perf.bp_full) {
            std::fprintf(stderr,
                         "[perf] agent broadphase fell back %llu times over %u ticks "
                         "(%.1f bodies a tick)\n",
                         static_cast<unsigned long long>(perf.bp_full), perf.ticks,
                         static_cast<double>(perf.bp_full) / n);
        }
        perf = Perf{perf.every, 0, 0, 0, 0, 0, 0};
    }

    // A stand-in for Lua, kept only so PARAM_UPDATE replication can be tested
    // without a script attached. Sector motion no longer needs an equivalent:
    // maps/scripts/init.lua drives it.
    bool param_test = false;
    void drive_param_test(u32 li) {
        if (!param_test) return;
        Level& L = level(li);
        if (L.world.tick() % 120 != 0) return;
        const i32 id = physics::param_id_by_name("move_speed");
        if (id < 0) return;
        const bool fast = (L.world.tick() / 120) % 2 == 1;
        const fx want = fast ? fx::from_int(16) : fx::from_int(8);
        for (auto& [sid, s] : sessions) {
            if (!s.has_agent || s.level != li) continue;
            if (L.world.params(s.agent).move_speed == want) continue;
            set_param(li, s.agent, static_cast<u16>(id), want.raw);
            std::fprintf(stderr, "[srv] tick %u: agent %u move_speed -> %lld (epoch %u)\n",
                         L.world.tick(), s.agent,
                         static_cast<long long>(want.raw >> 32), L.param_epoch[s.agent]);
        }
    }

    // Load one map as a new level, appended to the progression.
    //
    // Each level owns its MapData view, which used to be a function-local
    // `static` -- fine when there was exactly one map and a silent aliasing bug
    // the moment there were two.
    // A level this server will host. No file is opened: a declaration is a
    // name and a place, and the four megabytes are the loader's business.
    void declare_level(const char* path) {
        LevelSlot slot;
        slot.path = path;
        levels.push_back(std::move(slot));
    }

    // READ THE WORLD'S LAYOUT and bind it to the levels already declared.
    //
    // Bound by NAME -- the basename the level list shows -- because that is
    // what an author has in front of them. A cell naming a map this server did
    // not load is refused rather than ignored: it is almost always a typo, and
    // the alternative is a neighbour that silently never loads.
    bool load_metamap(const char* path) {
        std::FILE* f = std::fopen(path, "rb");
        if (f == nullptr) {
            std::fprintf(stderr, "[srv] cannot open metamap %s\n", path);
            return false;
        }
        std::string text;
        char chunk[4096];
        size_t got = 0;
        while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) text.append(chunk, got);
        std::fclose(f);

        meta = server::world::parse_metamap(text.data(), text.size());
        for (const std::string& problem : meta.problems) {
            std::fprintf(stderr, "[srv] %s: %s\n", path, problem.c_str());
        }

        u32 placed = 0;
        for (const server::world::MetaCell& c : meta.cells) {
            i32 found = -1;
            for (u32 i = 0; i < level_count(); ++i) {
                if (c.map == level_name(i)) { found = static_cast<i32>(i); break; }
            }
            if (found < 0) {
                std::fprintf(stderr, "[srv] %s places '%s', which is not a level here\n",
                             path, c.map.c_str());
                meta.problems.push_back("'" + c.map + "' is not a level here");
                continue;
            }
            LevelSlot& slot = levels[static_cast<u32>(found)];
            slot.cell.on_grid = true;
            slot.cell.x = c.x;
            slot.cell.y = c.y;
            ++placed;
        }
        if (!meta.ok()) {
            std::fprintf(stderr, "[srv] refusing to start on a metamap it could not read\n");
            return false;
        }
        std::fprintf(stderr,
                     "[srv] metamap %s: %u of %u level(s) placed, radius %u\n",
                     path, placed, level_count(), meta.radius);
        // A PITCH COMPOSES THE LEVELS IN WORLD SPACE, so the ones on the grid
        // share one world: a region. Walking from one into the next is then
        // walking between two rooms. Without a pitch the grid is only a
        // residency hint and every level keeps a world of its own.
        if (placed > 0 && meta.pitch_x.raw > 0 && meta.pitch_y.raw > 0) {
            region_ = std::make_unique<Host>();
            region_->region_store = std::make_unique<geom::MapStorage>();
            region_->store = region_->region_store.get();
            region_->view = &region_->region_view;
            std::fprintf(stderr, "[srv] region: the %u level(s) on the grid share one world\n",
                         placed);
        }
        return true;
    }

    // THE REGION: one world for every level on the metamap's grid (see Host).
    std::unique_ptr<Host> region_;
    bool in_region(u32 li) const {
        return region_ && li < levels.size() && levels[li].cell.on_grid;
    }
    // How many levels are in the region right now.
    u32 region_live_count() const {
        u32 n = 0;
        for (u32 i = 0; i < levels.size(); ++i) if (in_region(i) && levels[i].live) ++n;
        return n;
    }
    // Whether two levels' bodies are in the same world: one level, or two
    // levels of the region.
    bool same_host(u32 a, u32 b) const {
        if (a == b) return true;
        return in_region(a) && in_region(b);
    }
    // Whether session `s` has a body in the world level `li` is simulated in.
    bool same_world(const Session& s, u32 li) const {
        return s.has_agent && same_host(s.level, li);
    }
    // A level's spawn point, in its world's coordinates.
    fx3 spawn_of(const Level& L, u32 li) const {
        const fx3 p = L.map_bytes.empty()
                          ? fx3{fx::from_int(10), fx::from_int(10), fx::from_int(2)}
                          : L.map_store.spawn_for_tag(spawn_tag, spawn_tag_len);
        if (!in_region(li)) return p;
        const fx2 o = origin_of(li);
        return fx3{p.x + o.x, p.y + o.y, p.z};
    }

    // Bring a declared level's payload into memory.
    //
    // Everything that used to happen once at startup for every map happens here
    // for one map, when somebody is near it: the file, the nav graph, the Lua
    // VM and the map's own scripts. Idempotent, so a caller may ask without
    // checking.
    bool load_level(u32 li) {
        if (li >= levels.size()) return false;
        if (levels[li].live) return true;

        LevelSlot& slot = levels[li];
        const char* path = slot.path.c_str();
        auto lp = std::make_unique<Level>(in_region(li) ? region_.get() : nullptr);
        Level& L = *lp;

        std::FILE* f = std::fopen(path, "rb");
        if (!f) { std::fprintf(stderr, "[srv] cannot open map %s\n", path); return false; }
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n <= 0) { std::fclose(f); return false; }
        L.map_bytes.resize(static_cast<size_t>(n));
        const size_t got = std::fread(L.map_bytes.data(), 1, L.map_bytes.size(), f);
        std::fclose(f);
        if (got != L.map_bytes.size()) return false;

        ByteReader r(L.map_bytes.data(), L.map_bytes.size());
        const geom::MapLoadStatus st = geom::load_map(r, L.map_store);
        if (st != geom::MapLoadStatus::Ok) {
            std::fprintf(stderr, "[srv] map %s rejected: %s\n", path,
                         geom::map_load_status_text(st));
            return false;
        }

        L.view = L.map_store.view();
        if (L.own) {
            L.world.set_map(&L.view);
            L.world.set_map_storage(&L.map_store);
        } else {
            // INTO THE REGION, at its cell's origin, joined to whatever it
            // meets. The level keeps its own parse for its script and spawns.
            Host& H = *L.host;
            const bool first = region_live_count() == 0;
            if (H.store->add_chunk(li, L.map_store, origin_of(li)) == geom::NO_CHUNK) {
                std::fprintf(stderr, "[srv] level %u (%s) does not fit in the region's world\n",
                             li, path);
                return false;
            }
            H.region_view = H.store->view();
            H.world.set_map(&H.region_view);
            H.world.set_map_storage(H.store);
            if (first) H.world.set_tick(clock_);
            std::fprintf(stderr, "[srv] level %u joined the region: %u edge(s) stitched\n",
                         li, H.store->stitch_count());
        }
        L.map_hash = L.map_store.hash();

        std::fprintf(stderr, "[srv] level %u  %s: %u sectors, %u sides, hash %016llX\n",
                     li, path, L.view.sector_count, L.view.side_count,
                     static_cast<unsigned long long>(L.map_hash));

        // THE BROADPHASE, reported because its absence is invisible and
        // expensive. Without the sector grid every point query scans every
        // sector, which on a 180-sector Doom level is the difference between a
        // playable tick and one that misses its deadline -- and the only
        // symptom is "the server keeps falling behind", which names nothing.
        // Bucket occupancy, which is what the grid is actually worth. A grid
        // that exists but puts forty sectors in the cell you are standing in
        // saves nothing, and reports itself as working.
        if (L.view.grid.usable) {
            u32 worst = 0, total = 0, cells = 0;
            const u32 n = static_cast<u32>(L.view.grid.cols) * L.view.grid.rows;
            for (u32 c = 0; c < n; ++c) {
                const u32 k = L.view.grid.start[c + 1] - L.view.grid.start[c];
                total += k;
                if (k > 0) { ++cells; if (k > worst) worst = k; }
            }
            std::fprintf(stderr,
                "[srv]   grid: %ux%u, %u items, %.1f avg per occupied cell, %u worst\n",
                L.view.grid.cols, L.view.grid.rows, total,
                cells ? static_cast<double>(total) / cells : 0.0, worst);
        }
        if (!L.view.grid.usable) {
            std::fprintf(stderr,
                "[srv]   NO SECTOR GRID: point queries will scan all %u sectors. "
                "The map is too large or too sprawling for the grid's fixed "
                "capacity (geom::MAX_GRID_ITEMS).\n", L.view.sector_count);
        }

        // Nav graph, plus a reachability report from the default spawn. This is
        // a map-authoring signal, not a runtime one: "17 of 40 sectors are
        // reachable" is something the author wants to hear at load, and is
        // otherwise only discovered by watching an enemy fail to path.
        if (!L.nav_graph.build(*L.host->view)) {
            std::fprintf(stderr, "[srv] map has more sectors than the nav graph holds\n");
            return false;
        }
        L.nav_heights = 0;
        report_reachability(L, li);

        // ON THE SERVER'S CLOCK, before anything can read it. See clock_. A
        // region's world keeps one clock for all of its levels (set above when
        // it received its first).
        if (L.own) L.world.set_tick(clock_);

        slot.live = std::move(lp);

        // Its scripts, which used to be a second pass over every level at
        // startup. A level whose scripts fail is refused outright rather than
        // hosted without them: a map without its actions is a building with no
        // doors, and the loader that reported that at startup must go on
        // reporting it now that the load has moved.
        // A VM only where there is something to run in it. `scripts_enabled` is
        // the operator's veto; what decides otherwise is whether anybody named
        // a script -- the operator for every level, or this map for itself.
        const bool wants_scripts =
            scripts_enabled &&
            (!shared_scripts.empty() || L.map_store.script_count() > 0);
        if (wants_scripts &&
            !start_scripts(li, shared_scripts.data(),
                           static_cast<u32>(shared_scripts.size()))) {
            slot.live.reset();
            return false;
        }
        return true;
    }

    // Drop a level's payload, keeping everything a declaration is.
    //
    // THE STATE IS LOST, and that is the accepted answer for now: the doors it
    // had opened are shut again, its switches are unthrown, and the monsters it
    // had lost are back. A player who leaves and returns finds it as the map
    // author wrote it, which is what Doom does when you re-enter a level and is
    // not what a Metroid does.
    //
    // THE SEAM FOR THE OTHER ANSWER IS HERE, and deliberately in one place: a
    // level's persistent state would be serialised out of `slot.live` on this
    // line and read back in load_level, and nothing else in the server would
    // have to know. The clock is already treated that way -- it is the one
    // piece of state that has to survive, because the wire is built on it.
    void unload_level(u32 li) {
        if (li >= levels.size() || !levels[li].live) return;
        LevelSlot& slot = levels[li];
        std::fprintf(stderr, "[srv] level %u (%s) unloaded at tick %u\n",
                     li, slot.path.c_str(), slot.live->world.tick());
        Level& L = *slot.live;
        if (!L.own) {
            // OUT OF THE REGION, taking with it everything its script spawned:
            // a level that comes back runs its script again and places them
            // afresh, which is what Doom does with a level you left.
            Host& H = *L.host;
            for (u32 e = 0; e < sim::MAX_ENTITIES; ++e) {
                if (H.owner[e] != li) continue;
                H.owner[e] = NO_OWNER;
                if (sim::World::is_prop(e)) {
                    if (H.world.prop(e).active) H.world.despawn_prop(e);
                } else if (H.world.agent(e).active) {
                    H.effects.clear_agent(e);
                    H.world.despawn(e);
                }
            }
            if (const geom::Chunk* c = H.store->find_chunk(li)) {
                H.world.motions().forget_sectors(c->sectors.first, c->sectors.count);
            }
            H.store->remove_chunk(li);
            H.region_view = H.store->view();
            // Motion indices shifted with the removal: tell everybody again.
            H.sent_motion_count = 0;
            H.nav_graph.build(H.region_view);
            H.nav_heights = 0;
        }
        slot.live.reset();
    }

    // WHICH LEVELS MUST BE IN MEMORY, and making it so.
    //
    // Run when the answer can have changed -- somebody joined, left, or walked
    // through an exit -- and not per tick: the set is a function of who is
    // where, and that does not change sixty times a second.
    void refresh_residency() {
        const u32 n = static_cast<u32>(levels.size());
        meta_cells.assign(n, server::world::LevelCell{});
        meta_occupied.assign(n, false);
        for (u32 i = 0; i < n; ++i) meta_cells[i] = levels[i].cell;
        for (u32 i = 0; i < n; ++i) meta_occupied[i] = level_occupied(i);

        // THE ENTRY LEVEL IS ALWAYS OCCUPIED, whether or not anybody is
        // standing in it.
        //
        // Every session is placed in level 0 by the join, so "nobody is there"
        // is never true for long -- and a policy that took it literally
        // unloaded the map every arriving player is about to be put into, then
        // loaded it again inside their handshake. On an empty server it went
        // further: the entry map was dropped the moment it finished loading,
        // and the first join had to build it from scratch before it could
        // answer HELLO.
        //
        // Costs one level plus its neighbours, permanently, which is the price
        // of a join that does not stall.
        if (n > 0) meta_occupied[0] = true;

        server::world::residency(meta_cells, meta_occupied, meta.radius, meta_want);

        for (u32 i = 0; i < n; ++i) {
            if (meta_want[i] && !levels[i].live) {
                if (!load_level(i)) {
                    std::fprintf(stderr,
                        "[srv] level %u (%s) could not be loaded; it stays out of "
                        "reach until it can\n", i, levels[i].path.c_str());
                }
            } else if (!meta_want[i] && levels[i].live) {
                unload_level(i);
            }
        }

        // WHOSE VIEW JUST CHANGED. A level coming or going is somebody else's
        // doing -- they walked into a cell, or out of the last one keeping a
        // map resident -- so the observers who can see it have to be told
        // without any of them having moved. A diff in both directions, so a
        // pass where nothing changed sends nothing.
        for (auto& [sid, sess] : sessions) {
            if (!sess.hello_done) continue;
            sync_neighbours(sess, sid);
        }
    }

    // How much of the map a standard body can walk to from the spawn.
    //
    // A flood fill over the same edge test A* uses, so it reports what pathing
    // would actually find rather than what the topology suggests. Sectors are
    // counted once; the queue is the caller's stack-free scratch.
    void report_reachability(Level& L, u32 li) {
        const geom::MapData& map = *L.host->view;
        static u8  seen[nav::MAX_NAV_NODES];
        static u32 queue[nav::MAX_NAV_NODES];
        for (u32 i = 0; i < L.nav_graph.node_count(); ++i) seen[i] = 0;

        const fx3 origin = spawn_of(L, li);
        const i32 start = geom::find_sector(map, fx2{origin.x, origin.y});
        if (start < 0) {
            std::fprintf(stderr, "[srv] spawn '%s' is not inside any sector\n", spawn_tag);
            return;
        }

        const nav::NavBody body;
        u32 head = 0, tail = 0, found = 1;
        queue[tail++] = static_cast<u32>(start);
        seen[start] = 1;
        while (head < tail) {
            const u32 cur = queue[head++];
            const nav::NavNode& n = L.nav_graph.node(cur);
            for (u32 k = 0; k < n.edge_count; ++k) {
                const nav::NavEdge& e = L.nav_graph.edge(n.first_edge + k);
                if (seen[e.to]) continue;
                if (!L.nav_graph.edge_is_open(map, cur, e, body)) continue;
                seen[e.to] = 1;
                queue[tail++] = e.to;
                ++found;
            }
        }

        std::fprintf(stderr, "[srv] nav: %u nodes, %u edges; %u of %u sectors reachable "
                             "on foot from spawn '%s'\n",
                     L.nav_graph.node_count(), L.nav_graph.edge_count(), found,
                     L.nav_graph.node_count(), spawn_tag);
        if (found < L.nav_graph.node_count()) {
            // Not an error. Sectors reachable only by jumping, grappling or a
            // lift at the top of its travel are legitimate level design, and
            // the walk-only graph is expected to miss them (design §3.2.2:
            // enemies do not jump). Worth saying out loud all the same.
            std::fprintf(stderr, "[srv] nav: %u sector(s) need more than walking to reach\n",
                         L.nav_graph.node_count() - found);
        }
    }

    // The four-byte length prefix every control frame starts with, written once
    // the body's size is known. send_map did this inline and was the only
    // sender that needed it; it is now three.
    static void finish_frame(u8* msg, size_t len_pos, size_t end) {
        const u32 body = static_cast<u32>(end - len_pos - 4);
        msg[len_pos]     = static_cast<u8>(body & 0xFF);
        msg[len_pos + 1] = static_cast<u8>((body >> 8) & 0xFF);
        msg[len_pos + 2] = static_cast<u8>((body >> 16) & 0xFF);
        msg[len_pos + 3] = static_cast<u8>((body >> 24) & 0xFF);
    }

    void send_map(Level& L, u32 id) {
        const std::vector<u8>& map_bytes = L.map_bytes;
        if (map_bytes.empty()) return;
        for (size_t off = 0; off < map_bytes.size(); off += MAP_CHUNK_BYTES) {
            const size_t n = map_bytes.size() - off < MAP_CHUNK_BYTES
                                 ? map_bytes.size() - off
                                 : MAP_CHUNK_BYTES;
            std::vector<u8> msg(n + 32);
            ByteWriter w(msg.data(), msg.size());
            const size_t len_pos = w.mark();
            w.u32v(0);
            w.u8v(proto::MSG_MAP_DATA);
            w.u32v(static_cast<u32>(map_bytes.size()));
            w.u32v(static_cast<u32>(off));
            w.bytes(map_bytes.data() + off, n);
            const size_t end = w.size();
            finish_frame(msg.data(), len_pos, end);
            link.send_control(id, msg.data(), end);
        }
    }

    // WHAT THIS OBSERVER SHOULD BE ABLE TO SEE ACROSS ITS OWN CELL BOUNDARY.
    //
    // Every live level within the metamap's radius of the one it is standing
    // in, itself excluded. An unplaced level has no neighbours and is nobody's
    // neighbour -- `on_grid` is false and the Chebyshev test below never runs
    // for it -- so a server with no metamap produces an empty set here for
    // everybody, which is exactly the behaviour that existed before.
    void neighbours_of(u32 li, std::vector<u32>& out) const {
        out.clear();
        if (li >= levels.size()) return;
        const server::world::LevelCell& home = levels[li].cell;
        if (!home.on_grid) return;
        const i32 r = static_cast<i32>(meta.radius);
        for (u32 i = 0; i < levels.size(); ++i) {
            if (i == li || !levels[i].live) continue;
            const server::world::LevelCell& c = levels[i].cell;
            if (!c.on_grid) continue;
            // Chebyshev, the same shape residency() keeps resident. Using a
            // different one here would mean drawing a level that is not loaded
            // or loading one that is never drawn.
            const i32 dx = c.x - home.x, dy = c.y - home.y;
            if ((dx < 0 ? -dx : dx) <= r && (dy < 0 ? -dy : dy) <= r) out.push_back(i);
        }
    }

    // SEND WHAT CHANGED, and nothing else.
    //
    // A map is tens of kilobytes; re-sending the same eight every time a player
    // walks through a door would be a megabyte of reliable traffic for a view
    // that did not change. So this is the set difference between what the
    // session holds and what it should (world/neighbours.hpp), in both
    // directions.
    //
    // RUNS WITHOUT A PITCH TOO. Nothing is then wanted -- there is no offset a
    // neighbour could be drawn at -- but the level just left is still held, by
    // the rule, until it is dropped. Skipping the diff would leave every level
    // a player had ever visited held on both sides for the rest of the session.
    void sync_neighbours(Session& s, u32 id) {
        std::vector<server::world::WantedLevel> want;
        if (meta.pitch_x.raw != 0 || meta.pitch_y.raw != 0) {
            std::vector<u32> near;
            neighbours_of(s.level, near);
            const fx2 home = origin_of(s.level);
            for (const u32 li : near) {
                const fx2 o = origin_of(li);
                // RELATIVE, and that is the whole reason the client never needs
                // to know its own cell: it draws the neighbour at a translation.
                want.push_back(server::world::WantedLevel{li, (o.x - home.x).raw, (o.y - home.y).raw});
            }
        }
        for (const auto& m : server::world::diff_neighbours(s.held, want, s.level)) {
            u8 msg[32];
            ByteWriter w(msg, sizeof(msg));
            const size_t len_pos = w.mark();
            w.u32v(0);
            if (m.say == server::world::NeighbourSay::Drop) {
                w.u8v(proto::MSG_NEIGHBOUR_DROP);
                w.u16v(static_cast<u16>(m.level));
            } else {
                // A MOVE for a level already held: walking one cell changes
                // where the cells around you are far more than which ones they
                // are, and the client has those built.
                w.u8v(m.say == server::world::NeighbourSay::Move ? proto::MSG_NEIGHBOUR_MOVE
                                                                   : proto::MSG_NEIGHBOUR_MAP);
                w.u16v(static_cast<u16>(m.level));
                w.i64v(m.dx);
                w.i64v(m.dy);
            }
            finish_frame(msg, len_pos, w.size());
            link.send_control(id, msg, w.size());
            // Immediately after the selector, which is what makes the stream
            // that follows unambiguous on a reliable ordered channel.
            if (m.say == server::world::NeighbourSay::Map) {
                Level& N = *levels[m.level].live;
                send_map(N, id);
                // IN A REGION the client SIMULATES what it holds, not only
                // draws it: so the doors already open in it, and the switches
                // already thrown, follow its map exactly as its own level's
                // follow a WELCOME.
                if (same_world(s, m.level)) {
                    send_motion_sets(N, id);
                    for (const auto& t : N.side_textures) send_side_texture(s, m.level, t);
                }
            }
        }
    }

    std::map<u32, Session> sessions;   // ordered: iteration order must not vary

    // How many level-ticks were skipped because nobody was in them. A counter
    // rather than silence: "the server is fast now" and "the server stopped
    // simulating most of the game" look identical from outside, and only one of
    // them is what was intended.
    u64  idle_levels_skipped_ = 0;
    bool idle_skip_reported_ = false;
    u32                  snapshot_divisor = 2;   // 60 Hz sim, 30 Hz snapshots

    // Put a session into a level: spawn it, tell it where it is, hand it the
    // geometry and the rules.
    //
    // ONE routine for joining and for changing map, because they are the same
    // event from the client's side -- "here is a world, here is who you are in
    // it, here is everything you need to predict". Two implementations would
    // drift, and the way they would drift is that the second one forgets a
    // message and the symptom is a client predicting against the wrong map.
    //
    // Returns false only when the target level is full.
    //
    // Always at a spawn point. Walking between two levels of the region is not
    // a placement at all (update_region_homes); this is the join, and exits.
    bool place_in_level(Session& s, u32 id, u32 li) {
        if (li >= level_count()) {
            std::fprintf(stderr, "[srv] session %u: level %u does not exist\n", id, li);
            return false;
        }
        // ON DEMAND. The two callers are the join and a map change, which is
        // every way a session enters a level -- so this is the one place a
        // level has to be in memory by, and asking here is what lets every
        // other level stay out of it.
        //
        // REPORTED when it fails, because a level that will not load is now a
        // runtime event rather than a startup one: before levels were loaded on
        // demand a bad map refused the server, and the failure a player sees
        // instead has to name itself somewhere.
        if (!load_level(li)) {
            std::fprintf(stderr, "[srv] session %u: level %u (%s) would not load\n",
                         id, li, levels[li].path.c_str());
            return false;
        }
        Level& L = level(li);
        // The level this client's home map is, before this changes it.
        const u32  from_level = s.level;
        const bool had_home   = s.has_home;

        // Leave the old one first. A player who has moved on must stop
        // occupying a slot in the map they left -- otherwise their body stands
        // there forever, in everyone else's snapshots, being collided with.
        if (s.has_agent) {
            Level& old = level(s.level);
            old.effects.clear_agent(s.agent);
            old.world.despawn(s.agent);
            s.has_agent = false;

            // Panels do not travel between levels.
            //
            // Each level runs its own VM, so a panel opened by the old level's
            // script is held by a script that is about to stop being consulted
            // about this player -- its handlers are in a different Lua state
            // and will never see another event from it. Left alone the window
            // stays on screen, permanently dead, and the new level's script
            // opens a second one beside it.
            for (const auto& p : s.open_ui) send_ui_close(s, p.ui_id);
            s.open_ui.clear();

            // And the input buffer, or the last thing they did before touching
            // the exit is repeated forever in the new level. See
            // Session::reset_input.
            s.reset_input();
        }

        const fx3 spawn_pos = spawn_of(L, li);
        const u32 agent = L.world.spawn(spawn_pos);
        if (agent >= sim::MAX_AGENTS) {
            std::fprintf(stderr, "[srv] session %u: level %u has no free body slot\n", id, li);
            return false;
        }

        // Full health on arrival, including on a map change: a slot is reused
        // as players come and go, and inheriting the last occupant's health
        // would be a bug that only shows up on a busy server. (Walking between
        // two levels of the region is not an arrival and heals nobody.)
        L.hp[agent] = 100;

        // The body the MAP is built for, before anything captures this agent's
        // parameters as its base.
        //
        // A converted Doom map is at 1/48, where Doom's 56-unit-tall player is
        // 1.167 of our units and the engine's default 2.0 body is 96 Doom units
        // -- taller than most of the doorways it is expected to walk through.
        // Both fields are replicated parameters (ids 0 and 1), so the full
        // PARAM_UPDATE below carries them and prediction agrees from the first
        // tick. Per LEVEL, because two maps may be at different scales.
        physics::MoveParams& p = L.world.params_mut(agent);
        p.radius = L.map_store.body_radius();
        p.height = L.map_store.body_height();

        // Capture the authored parameters as this agent's base. Everything
        // effects do is computed from here, never from the live value --
        // see server/src/lua/effects.hpp for why that matters.
        L.effects.set_base(agent, L.world.params(agent));

        s.level = li;
        s.agent = agent;
        s.has_agent = true;
        // The snapshot bookkeeping is about a world this session has just left.
        // Carrying a baseline across a map change would delta this map's
        // entities against the last one's.
        s.forget_history();

        // WHETHER THE MAP FOLLOWS. Not if the client holds it already, which by
        // the rule in world/neighbours.hpp it does exactly when this level is
        // in `held` -- and the level just left joins `held` here, because its
        // map was this client's home map until a moment ago.
        const bool map_follows = !server::world::rehome(s.held, had_home, from_level, li);
        s.has_home = true;

        u8 buf[128];
        ByteWriter w(buf, sizeof(buf));
        const size_t len_pos = w.mark();
        w.u32v(0);   // patched below
        w.u8v(proto::MSG_WELCOME);
        w.u16v(proto::PROTOCOL_VERSION);
        w.u64v(0x444E5030u);                       // server_id
        w.u16v(proto::TICK_RATE_HZ);
        w.u16v(static_cast<u16>(sim::TICK_RATE_HZ / snapshot_divisor));
        w.u32v(L.world.tick());
        w.u32v(proto::Handle::make(agent + 1, L.world.generation(agent)).raw);
        w.u64v(L.map_hash);
        // Appended, so a reader of the older layout still reads the rest.
        w.u16v(static_cast<u16>(li));
        w.u8v(map_follows ? 1 : 0);
        // exact_arrival: always 0 now. A body walking between two levels of
        // the region is re-homed (MSG_HOME), never re-placed; this is a spawn.
        w.u8v(0);
        // Where the body is, in the WORLD's coordinates, on this tick.
        {
            const fx3 p = L.world.agent(agent).pos;
            w.i64v(p.x.raw);
            w.i64v(p.y.raw);
            w.i64v(p.z.raw);
        }
        // Where this level's own coordinates begin in the world's (zero for a
        // level simulated on its own), and whether it shares its world with
        // its neighbours -- then walking into one is a HOME, not a WELCOME.
        {
            const fx2 o = in_region(li) ? origin_of(li) : fx2{};
            w.i64v(o.x.raw);
            w.i64v(o.y.raw);
            w.u8v(in_region(li) ? 1 : 0);
        }
        const size_t end = w.size();
        const u32 body_len = static_cast<u32>(end - len_pos - 4);
        buf[len_pos]     = static_cast<u8>(body_len & 0xFF);
        buf[len_pos + 1] = static_cast<u8>((body_len >> 8) & 0xFF);
        buf[len_pos + 2] = static_cast<u8>((body_len >> 16) & 0xFF);
        buf[len_pos + 3] = static_cast<u8>((body_len >> 24) & 0xFF);
        link.send_control(id, buf, end);

        // The map follows WELCOME immediately, unless the client holds it. A
        // client must not start predicting before it has it (docs/protocol.md
        // 8.0) -- physics against no geometry is wrong on every tick.
        //
        // This used to be sent ALWAYS, because skipping it made correctness
        // depend on the server's belief about the client's bookkeeping, and
        // that belief was wrong three times in three ways. It is not a belief
        // any more: holding is defined by the protocol (world/neighbours.hpp),
        // both sides keep to it, and server/tests/test_neighbours.cpp tests it.
        if (map_follows) send_map(L, id);

        // THEN THE CELLS AROUND IT, including the level just left, which is
        // held and has no offset from this one yet. After this level's own map
        // because that is the one everything else is measured from.
        sync_neighbours(s, id);
        // Motion parameters follow the map. Parametric movers cost nothing
        // after this; the client computes them for any tick. A door already
        // open when you arrive is one of these.
        send_motion_sets(L, id);
        // And the switches already thrown in this level. A wall that changed
        // before you arrived is part of the world you are arriving in.
        for (const auto& t : L.side_textures) send_side_texture(s, li, t);
        // And the level's camera, if its script set one: the angle the map was
        // made for, and whether it may be turned away from.
        if (L.has_camera) send_camera(s, L.camera);
        // And the movement parameters. FULL, because a client arriving in a
        // level has no history to patch (docs/protocol.md 8.4). Sent before the
        // first snapshot so it is never predicting with rules it has not been
        // told.
        send_param_update(L, id, agent, true);
        return true;
    }

    // Bring up one level's script VM and load its scripts.
    //
    // The SHARED scripts first, then the map's own -- found beside its .dnpm
    // with the same basename, which is exactly where the Doom converter writes
    // it. A map without one is fine; a map with one gets its doors.
    bool start_scripts(u32 li, const char* const* shared, u32 shared_count) {
        Level& L = level(li);
        if (!L.vm.open(vm_config)) {
            std::fprintf(stderr, "[srv] cannot start Lua for %s: %s\n",
                         levels[li].path.c_str(), L.vm.last_error());
            return false;
        }
        L.ctx.world       = &L.world;
        L.ctx.commands    = &L.commands;
        L.ctx.effects     = &L.effects;
        // The WORLD'S store: live heights, motions, joins. A region level's
        // script addresses its own part of it through the frame below.
        L.ctx.map_store   = L.host->store;
        L.ctx.chunk       = L.own ? nullptr : L.host->store->find_chunk(li);
        L.ctx.origin      = L.own ? fx2{} : origin_of(li);
        L.ctx.owner       = L.own ? nullptr : L.host->owner;
        // Borrowed. The level owns its graph and rebuilds it on a shape
        // commit; a copy here would path over a map that no longer exists.
        L.ctx.nav         = &L.nav_graph;
        L.ctx.level       = li;
        L.ctx.map_changes = this;
        L.ctx.ui = this;
        L.ctx.hp = L.hp;
        L.ctx.avatar = L.avatar;
        L.ctx.levels = this;
        L.ctx.props       = &props;
        // Seeded from the map, so a scripted-discrete schedule is unpredictable
        // to a client but replayable by the server. Per level, or two maps
        // would draw the same "random" sequence.
        L.ctx.rng = Rng(L.map_hash ^ 0x9E3779B97F4A7C15ull, 11);
        // Installed BEFORE any chunk runs: a script's top level is allowed to
        // look at the world, and `dnp` being nil at load time would be a
        // baffling first experience.
        lua::open_world(L.vm.state(), &L.ctx);

        for (u32 k = 0; k < shared_count; ++k) {
            if (!L.vm.run_file(shared[k], shared[k])) {
                std::fprintf(stderr, "[srv] script failed: %s\n", L.vm.last_error());
                return false;
            }
        }

        // Then whatever the MAP asked for, in the order it asked, resolved
        // beside its own .dnpm.
        //
        // The map decides what it needs to run properly; the operator's
        // --script arguments are the house rules that apply to every map, and
        // they load first so a map's own file can build on them. A declared
        // script that is missing or fails is fatal for that level: a map whose
        // actions did not load is a building with no doors, and starting it
        // anyway would hide that behind "the switches don't work".
        const size_t slash = levels[li].path.find_last_of("/\\");
        const std::string dir = slash == std::string::npos ? std::string(".")
                                                           : levels[li].path.substr(0, slash);
        for (u32 i = 0; i < L.map_store.script_count(); ++i) {
            const geom::ScriptName& sn = L.map_store.script(i);
            const std::string path = dir + "/" + std::string(sn.name, sn.len);
            if (!L.vm.run_file(path.c_str(), path.c_str())) {
                std::fprintf(stderr, "[srv] %s asked for %s and it failed: %s\n",
                             levels[li].path.c_str(), path.c_str(), L.vm.last_error());
                return false;
            }
            std::fprintf(stderr, "[srv] level %u: %s (declared by the map)\n", li, path.c_str());
        }

        L.scripted = true;
        return true;
    }

    void on_open(u32 id) {
        Session s;
        s.reset();
        s.id = id;
        sessions[id] = s;
        std::fprintf(stderr, "[srv] session %u opened\n", id);
    }

    void on_close(u32 id, u16 reason) {
        auto it = sessions.find(id);
        if (it == sessions.end()) return;   // defined no-op
        if (it->second.has_agent) {
            Level& L = level(it->second.level);
            L.effects.clear_agent(it->second.agent);
            L.world.despawn(it->second.agent);
        }
        const Session& c = it->second;
        const double avg = c.snapshots_sent ? static_cast<double>(c.bytes_sent) / c.snapshots_sent : 0.0;
        std::fprintf(stderr,
                     "[srv] session %u closed (reason %u, starved %llu ticks, "
                     "%llu snapshots, %.1f bytes avg, %llu entity-sends elided)\n",
                     id, reason,
                     static_cast<unsigned long long>(c.starved_ticks),
                     static_cast<unsigned long long>(c.snapshots_sent),
                     avg,
                     static_cast<unsigned long long>(c.entities_omitted));
        sessions.erase(it);
        // The last player in a level leaving is the commonest reason a level
        // stops being wanted, and the one that gives the memory back.
        refresh_residency();
    }

    // Send a panel to one client (docs/protocol.md §11.1). Reliable channel.
    void send_ui_open(Session& s, const proto::UiPanel& p) {
        u8 buf[8192];
        ByteWriter w(buf, sizeof(buf));
        const size_t len_pos = w.mark();
        w.u32v(0);
        w.u8v(proto::MSG_UI_OPEN);
        proto::encode_ui_open(w, p);
        if (!w.ok()) {
            // A panel too large to send is a script error, not a client one.
            // Named, because the alternative is a shop that silently never
            // opens and a script author with nothing to go on.
            std::fprintf(stderr, "[srv] session %u: UI panel %u too large to send\n",
                         s.id, p.ui_id);
            return;
        }
        const size_t end = w.size();
        const u32 body = static_cast<u32>(end - len_pos - 4);
        buf[len_pos]     = static_cast<u8>(body & 0xFF);
        buf[len_pos + 1] = static_cast<u8>((body >> 8) & 0xFF);
        buf[len_pos + 2] = static_cast<u8>((body >> 16) & 0xFF);
        buf[len_pos + 3] = static_cast<u8>((body >> 24) & 0xFF);
        link.send_control(s.id, buf, end);
    }

    // One widget's value, in a panel already open (docs/protocol.md §11.1).
    void send_ui_set(Session& s, u32 ui_id, u16 widget_id, i32 value) {
        u8 buf[32];
        ByteWriter w(buf, sizeof(buf));
        w.u32v(1 + 10);
        w.u8v(proto::MSG_UI_SET);
        w.u32v(ui_id);
        w.u16v(widget_id);
        w.i32v(value);
        if (w.ok()) link.send_control(s.id, buf, w.size());
    }

    void send_ui_close(Session& s, u32 ui_id) {
        u8 buf[32];
        ByteWriter w(buf, sizeof(buf));
        w.u32v(1 + 4);
        w.u8v(proto::MSG_UI_CLOSE);
        w.u32v(ui_id);
        if (w.ok()) link.send_control(s.id, buf, w.size());
    }

    // An event from a client, on its way to a script (docs/protocol.md §11.2).
    //
    // Everything here is a refusal. The client picks `ui_id`, `widget_id` and
    // `value`, so a script must never be handed one it did not offer: a stale
    // id is a no-op, an unknown widget is dropped, and a value is clamped to
    // the range the widget itself declared. A script written against a UI is
    // not written against an adversary, which is why this layer has to be the
    // thing that makes that true.
    void on_ui_event(Session& s, const proto::UiEvent& ev) {
        proto::UiPanel* panel = s.find_ui(ev.ui_id);
        if (panel == nullptr) return;          // closed, or never opened

        const proto::UiWidget* widget = nullptr;
        for (u16 i = 0; i < panel->count; ++i) {
            if (panel->widgets[i].id == ev.widget_id) { widget = &panel->widgets[i]; break; }
        }
        if (widget == nullptr) return;
        // A label sends nothing, so an event naming one is not from a person.
        if (widget->kind == proto::UiWidgetKind::Label) return;

        i32 value = ev.value;
        switch (widget->kind) {
            case proto::UiWidgetKind::Button: value = 1; break;
            case proto::UiWidgetKind::Toggle: value = value != 0 ? 1 : 0; break;
            case proto::UiWidgetKind::Select:
                if (value < 0 || value >= static_cast<i32>(widget->option_count)) return;
                break;
            case proto::UiWidgetKind::Slider:
                if (value < widget->lo) value = widget->lo;
                if (value > widget->hi) value = widget->hi;
                break;
            default: return;
        }

        if (!s.has_agent) return;
        Level& L = level(s.level);
        if (!L.vm.is_open()) return;
        for (u32 h = 0; h < L.vm.hook_count("on_ui_event"); ++h) {
            if (!L.vm.begin_hook_at("on_ui_event", h)) continue;
            lua::push_entity(L.vm.state(), L.ctx, s.agent);
            lua_pushnumber(L.vm.state(), static_cast<double>(ev.ui_id));
            lua_pushnumber(L.vm.state(), static_cast<double>(ev.widget_id));
            lua_pushnumber(L.vm.state(), static_cast<double>(value));
            L.vm.end_hook("on_ui_event", L.world.tick(), 4);
        }
    }

    void on_control(u32 id, const u8* data, size_t len) {
        auto it = sessions.find(id);
        if (it == sessions.end()) return;
        Session& s = it->second;

        // Control messages arrive already de-framed by the edge, one per call.
        ByteReader r(data, len);
        const u32 msg_len = r.u32v();
        const u8  type = r.u8v();
        (void)msg_len;
        if (!r.ok()) return;

        if (type == proto::MSG_UI_EVENT) {
            proto::UiEvent ev;
            if (!proto::decode_ui_event(r, ev)) return;
            on_ui_event(s, ev);
            return;
        }

        if (type == proto::MSG_HELLO) {
            const u16 version = r.u16v();
            if (!r.ok() || version != proto::PROTOCOL_VERSION) {
                // Version 0 is UNSTABLE: there is no compatibility range, so a
                // mismatch is rejected outright rather than negotiated.
                u8 buf[64];
                ByteWriter w(buf, sizeof(buf));
                w.u32v(1 + 2);
                w.u8v(proto::MSG_REJECT);
                w.u16v(1);   // reason 1: version mismatch
                link.send_control(id, buf, w.size());
                link.close_session(id, 1);
                return;
            }

            if (!place_in_level(s, id, 0)) {
                u8 buf[64];
                ByteWriter w(buf, sizeof(buf));
                w.u32v(1 + 2);
                w.u8v(proto::MSG_REJECT);
                w.u16v(2);   // reason 2: server full
                link.send_control(id, buf, w.size());
                link.close_session(id, 2);
                return;
            }
            s.hello_done = true;
            std::fprintf(stderr, "[srv] session %u -> level 0 agent %u\n", id, s.agent);
            // The first arrival is what pulls level 0's neighbours in. Their
            // own level is already loaded -- place_in_level did that -- so this
            // is about what they can walk to next.
            refresh_residency();
        }
    }

    void on_datagram(u32 id, const u8* data, size_t len) {
        auto it = sessions.find(id);
        if (it == sessions.end() || !it->second.hello_done) return;

        ByteReader r(data, len);
        proto::InputBundle b;
        if (!proto::decode_input_bundle(r, b)) {
            // A malformed datagram is dropped silently. It is unreliable
            // transport: the next one is already on its way, and there is
            // nothing useful to tell the client that it will not learn faster
            // from the next snapshot.
            return;
        }
        // The world tick this session's level is at, so ingest can tell input
        // meant for THIS world from input still in flight from the last one.
        //
        // BOUNDS-CHECKED, because level() is not: it is `*levels[i]` with no
        // range test, and every other use compares `s.level` rather than
        // indexing with it. A datagram arriving for a session whose level is
        // out of range -- during teardown, or from a client talking to a server
        // that has since dropped a level -- would be an out-of-bounds read on
        // the network path, which is the worst place to have one.
        Session& sess = it->second;
        if (sess.level >= level_count()) return;
        sess.ingest(b, level(sess.level).world.tick());
    }

    // Crush and pin events from the tick just run (design §3.1).
    //
    // Reported, not acted on. What happens to a crushed entity is a Lua
    // decision and Lua arrives in Phase 4; until then this is where that
    // callback will be dispatched from, and logging it keeps the detection
    // observable rather than silently discarded.
    //
    // Rate-limited per entity: a body held under a closing ceiling raises an
    // event on every tick it stays there, and a line per tick per player would
    // bury everything else in the log within seconds.
    void report_crushes(Level& L) {
        bool now[sim::MAX_AGENTS]{};
        const sim::CrushLog& log = L.world.crushes();
        for (u32 i = 0; i < log.count(); ++i) {
            const sim::CrushEvent& e = log.at(i);
            if (e.entity >= sim::MAX_AGENTS) continue;
            now[e.entity] = true;
            if (L.crush_seen[e.entity]) continue;   // already reported this episode
            L.crush_seen[e.entity] = true;
            L.crush_reported_tick[e.entity] = L.world.tick();
            std::fprintf(stderr, "[srv] tick %u: agent %u %s by sector %u (depth %.3f)\n",
                         L.world.tick(), e.entity,
                         e.kind == sim::CrushKind::Vertical ? "crushed" : "pinned",
                         e.sector, static_cast<double>(e.depth.raw) / 4294967296.0);
        }
        // An episode ends when the geometry lets go, so escaping and being
        // caught again reports twice. To the high-water mark: a slot that was
        // never active was never crushed either, and this runs every tick.
        for (u32 i = 0; i < L.world.agent_high_water(); ++i) {
            if (!now[i] && L.crush_seen[i]) {
                std::fprintf(stderr, "[srv] tick %u: agent %u released after %u ticks\n",
                             L.world.tick(), i, L.world.tick() - L.crush_reported_tick[i]);
                L.crush_seen[i] = false;
            }
        }
        if (log.dropped() > 0) {
            // Never silently cap -- the same rule R10 applies to snapshots.
            std::fprintf(stderr, "[srv] tick %u: %u crush events dropped (log full)\n",
                         L.world.tick(), log.dropped());
        }
    }

    // Hand this tick's engine-detected events to Lua.
    //
    // The engine detects, Lua decides -- the same division §3.1 draws for
    // crushing and §3.2.2 for interaction. `find_interact_side` answers "which
    // side did this agent reach for"; whether that side is a door, a switch or
    // a blank wall is a gameplay question, and gameplay is script.
    //
    // Dispatched in a fixed order (crushes, then interactions, each in the
    // order the log recorded, which is ascending agent index) so that two
    // scripts racing for the same door resolve the same way on a replay.
    // WHOSE EVENT IS IT. In a world of its own, every event is the level's.
    // In the region: an event about a sector or side belongs to the level that
    // sector is in; one about a monster or prop to the level whose script
    // spawned it; one about a player to the level the player is standing in.
    bool owns_sector(const Level& L, u32 li, i32 sector) const {
        if (L.own) return true;
        return sector >= 0 && L.host->store->chunk_of_sector(static_cast<u32>(sector)) == li;
    }
    bool owns_entity(const Level& L, u32 li, u32 e) const {
        if (L.own) return true;
        if (e >= sim::MAX_ENTITIES) return false;
        const u32 o = L.host->owner[e];
        if (o != NO_OWNER) return o == li;
        // Nobody's (a player): whoever's ground it stands on.
        if (sim::World::is_prop(e)) return false;
        return owns_sector(L, li, L.world.agent(e).mv.sector);
    }

    void dispatch_events(Level& L, u32 li) {
        if (!L.vm.is_open()) return;
        const u32 t = L.world.tick();

        const sim::CrushLog& crushes = L.world.crushes();
        for (u32 i = 0; i < crushes.count(); ++i) {
            const sim::CrushEvent& e = crushes.at(i);
            if (e.entity >= sim::MAX_AGENTS) continue;
            // Only the first tick of an episode, matching what report_crushes
            // logs: a script that took damage sixty times a second while a
            // ceiling sat on someone would be a different design decision, and
            // one a script can make for itself from on_tick.
            if (L.crush_reported_tick[e.entity] != t || !L.crush_seen[e.entity]) continue;
            if (!owns_sector(L, li, static_cast<i32>(e.sector))) continue;
            for (u32 h = 0; h < L.vm.hook_count("on_crush"); ++h) {
                if (!L.vm.begin_hook_at("on_crush", h)) continue;
                lua::push_entity(L.vm.state(), L.ctx, e.entity);
                lua::push_sector(L.vm.state(), e.sector);
                lua_pushstring(L.vm.state(),
                               e.kind == sim::CrushKind::Vertical ? "vertical" : "pinned");
                L.vm.end_hook("on_crush", t, 3);
            }
        }

        const sim::InteractLog& uses = L.world.interactions();
        for (u32 i = 0; i < uses.count(); ++i) {
            const sim::InteractEvent& e = uses.at(i);
            if (e.entity >= sim::MAX_AGENTS || e.sector < 0) continue;
            if (!owns_sector(L, li, e.sector)) continue;
            const char* hook = e.kind == sim::TouchKind::Use ? "on_interact" : "on_cross";
            // The level's own side number: its script keys its doors by it.
            const i32 local_side = L.ctx.side_to_local(static_cast<i32>(e.side));
            for (u32 h = 0; h < L.vm.hook_count(hook); ++h) {
                if (!L.vm.begin_hook_at(hook, h)) continue;
                lua::push_entity(L.vm.state(), L.ctx, e.entity);
                // The SIDE INDEX, as a plain number, because that is the key a
                // generated script indexes its table of doors by. dnp.side(i)
                // turns it into neighbour and flags when the script wants them.
                lua_pushnumber(L.vm.state(), local_side);
                lua::push_sector(L.vm.state(), static_cast<u32>(e.sector));
                L.vm.end_hook(hook, t, 3);
            }
        }
        // Agents standing inside props. Reported every tick the overlap holds,
        // because "picked up once" is a decision the script makes by removing
        // the prop -- an engine-side edge trigger would only duplicate that, and
        // would get it wrong for anything that is meant to keep working while
        // you stand in it.
        const sim::PropEventLog& pe = L.world.prop_events();
        for (u32 i = 0; i < pe.count(); ++i) {
            const sim::PropEvent& e = pe.at(i);
            if (!owns_entity(L, li, e.prop)) continue;
            const char* hook = e.kind == sim::PropEventKind::Use ? "on_prop_use" : "on_touch";
            for (u32 h = 0; h < L.vm.hook_count(hook); ++h) {
                if (!L.vm.begin_hook_at(hook, h)) continue;
                lua::push_entity(L.vm.state(), L.ctx, e.agent);
                lua::push_prop(L.vm.state(), L.ctx, e.prop);
                L.vm.end_hook(hook, t, 2);
            }
        }
        // Swings that connected. One hook per target, in the order the engine
        // reported them, so a script that damages several things in one swing
        // does it in an order a replay reproduces.
        const sim::AttackLog& hits = L.world.attacks();
        for (u32 i = 0; i < hits.count(); ++i) {
            const sim::AttackEvent& e = hits.at(i);
            if (!owns_entity(L, li, e.target)) continue;
            for (u32 h = 0; h < L.vm.hook_count("on_attack"); ++h) {
                if (!L.vm.begin_hook_at("on_attack", h)) continue;
                lua::push_entity(L.vm.state(), L.ctx, e.attacker);
                // The target is an entity index, a prop or an agent. Each is
                // pushed as the handle type it is, so a script tells them apart
                // by asking rather than by comparing a number to MAX_AGENTS.
                if (sim::World::is_prop(e.target)) lua::push_prop(L.vm.state(), L.ctx, e.target);
                else                               lua::push_entity(L.vm.state(), L.ctx, e.target);
                L.vm.end_hook("on_attack", t, 2);
            }
        }
        if (hits.dropped() > 0) {
            std::fprintf(stderr, "[srv] tick %u: %u attack events dropped (log full)\n",
                         t, hits.dropped());
        }

        if (pe.dropped() > 0) {
            std::fprintf(stderr, "[srv] tick %u: %u prop events dropped (log full)\n",
                         t, pe.dropped());
        }

        if (uses.dropped() > 0) {
            std::fprintf(stderr, "[srv] tick %u: %u interact events dropped (log full)\n",
                         t, uses.dropped());
        }
    }

    // Recompute effective parameters from base and replicate what changed.
    //
    // From BASE every time, never incrementally: apply-then-remove has to be
    // exactly the identity, and in fixed point an incremental version is not
    // (effects.hpp). The diff against the live values is what keeps this from
    // sending a full parameter set every tick an effect is running.
    void refresh_effect_params(u32 li) {
        Level& L = level(li);
        // Effects are per BODY, so per world: in the region, once per tick for
        // all of its levels, dirtied by any of their scripts.
        bool dirty = false;
        bool any_vm = false;
        for (u32 k = 0; k < levels.size(); ++k) {
            if (!levels[k].live || !same_host(k, li)) continue;
            Level& K = level(k);
            if (!K.vm.is_open()) continue;
            any_vm = true;
            dirty = dirty || K.ctx.effects_dirty;
            K.ctx.effects_dirty = false;
        }
        if (!any_vm) return;
        const bool expired = L.effects.expire(L.world.tick());
        if (!expired && !dirty) return;
        for (auto& [id, s] : sessions) {
            if (!same_world(s, li)) continue;
            const u32 a = s.agent;

            const physics::MoveParams want = L.effects.compute(a);
            physics::MoveParams& live = L.world.params_mut(a);

            u16 changed[physics::PARAM_COUNT];
            u16 n = 0;
            for (u16 f = 0; f < physics::PARAM_COUNT; ++f) {
                const i64 target = physics::param_get_raw(want, f);
                if (physics::param_get_raw(live, f) == target) continue;
                physics::param_set_raw(live, f, target);
                changed[n++] = f;
            }
            if (n == 0) continue;

            // One epoch bump and one message for the whole batch, not one per
            // field: an effect touching four parameters is one change to the
            // rules, and four epochs would make the client look stale three
            // extra times for no reason.
            L.param_epoch[a] = static_cast<u8>(L.param_epoch[a] + 1);
            for (auto& [oid, os] : sessions) {
                if (os.hello_done && same_world(os, li)) send_param_update(L, oid, a, false, changed, n);
            }
        }
    }

    void flush_lua_commands(Level& L) {
        if (!L.vm.is_open()) return;
        const u32 now = L.world.tick();
        L.commands.apply(L.world, [&](u32 agent) {
            if (agent < sim::MAX_AGENTS) L.lua_touched_tick[agent] = now + 1;
        });
        if (L.commands.dropped() > 0) {
            // R10's rule again: never silently cap.
            std::fprintf(stderr, "[srv] tick %u: %u Lua commands dropped (queue full)\n",
                         now, L.commands.dropped());
            L.commands.clear();
        }
    }

    /**
     * Rebuild the nav graph when the geometry it describes has moved.
     *
     * WHAT WAS WRONG. nav_graph.build() ran once, at load. Every Doom door is
     * shut at that moment -- stored, as Doom stores them, as a sector whose
     * ceiling rests on its floor -- so the graph said the map was a handful of
     * rooms with no way between them, and went on saying it after every door in
     * the level had been opened. A monster could not path through a door it was
     * standing in.
     *
     * WHAT IS CHECKED. Only the sectors that can move: a fingerprint over the
     * floor and ceiling of each entry in the motion set, which is at most
     * MAX_MOVING_SECTORS values. Geometry that cannot move cannot invalidate
     * the graph, so there is nothing else to look at.
     *
     * THE THROTTLE is not an optimisation detail, it is the whole cost story: a
     * lift travels for about a second, and rebuilding on every tick of that is
     * sixty rebuilds to describe one journey whose only interesting states are
     * the two ends. At this interval a monster notices an opened door within a
     * quarter of a second, which is faster than it can walk to one.
     */
    void refresh_nav_if_stale(u32 li) {
        Level& L = level(li);
        const sim::SectorMotionSet& ms = L.world.motions();
        if (ms.count() == 0) return;

        // FNV-1a over the moving sectors' heights, in motion-set order. Order is
        // stable because the set is only appended to.
        u64 fp = 1469598103934665603ull;
        const auto mix = [&fp](i64 v) {
            for (int b = 0; b < 8; ++b) {
                fp ^= static_cast<u64>((v >> (b * 8)) & 0xFF);
                fp *= 1099511628211ull;
            }
        };
        for (u32 i = 0; i < ms.count(); ++i) {
            const u32 si = ms.at(i).sector;
            if (si >= L.host->view->sector_count) continue;
            mix(L.host->view->sector(si).floor_z.raw);
            mix(L.host->view->sector(si).ceil_z.raw);
        }

        if (fp == L.nav_heights) return;

        const u32 now = L.world.tick();
        if (L.nav_built_tick != 0 && now - L.nav_built_tick < NAV_REBUILD_TICKS) return;

        // Said once per level. A Doom map has lifts running all through it, so
        // a line per rebuild would be a log nobody reads -- but "the nav graph
        // is not what it was at load" is worth knowing once, because for a long
        // time it silently never was.
        if (L.nav_heights == 0) {
            std::fprintf(stderr, "[srv] %s: nav rebuilt -- geometry moved (tick %u)\n",
                         levels[li].path.c_str(), now);
        }
        L.nav_heights = fp;
        L.nav_built_tick = now;
        // A failure here leaves the previous graph in place, which is the older
        // answer rather than no answer -- and nav already reports at load when a
        // map has more sectors than it holds.
        L.nav_graph.build(*L.host->view);
    }

    // One level's tick. Every level runs every tick, in load order.
    //
    // In load order and not, say, "only levels with players in them", because a
    // level whose clock stopped while it was empty would hand the next arrival
    // a lift frozen mid-travel and a door that has been closing for four
    // minutes. Cheap enough that the question does not arise yet -- an empty
    // level with no movers is a handful of early-outs -- and the moment it is
    // not, the fix is to make an idle level cheap rather than to make time
    // conditional.
    void tick_level(u32 li) {
        Level& L = level(li);
        for (auto& [id, s] : sessions) {
            if (s.has_agent && s.level == li) L.world.set_input(s.agent, s.take());
        }
        // Lua runs OUTSIDE world.step(), always. The client predicts by running
        // the same step; anything Lua did has to reach it as replicated state,
        // never as a second implementation of physics.
        const u64 t_lua0 = perf.every ? now_us() : 0;
        if (L.vm.is_open()) {
            L.vm.begin_tick();
            // Timers and per-tick logic run BEFORE the step, in the same place
            // sector motion used to be driven from, and for the same reason:
            // what a snapshot reports must be what the tick actually ran with.
            L.vm.call_hook("on_tick", L.world.tick());
        }
        if (perf.every) perf.lua_us += now_us() - t_lua0;
        // After the callbacks, before the step: what the tick runs with is what
        // the snapshot will report.
        refresh_effect_params(li);

        drive_param_test(li);

        // Flush 1. Everything scripts asked for before the step lands here, so
        // physics never observes an entity moving underneath it mid-resolution.
        flush_lua_commands(L);

        const u64 t_step0 = perf.every ? now_us() : 0;
        L.world.step();
        if (perf.every) {
            perf.step_us += now_us() - t_step0;
            perf.bp_full += L.world.broadphase_full();
        }

        // A HASH TRACE, for comparing one run against another.
        //
        // The world's own state hash, per level, per tick. It exists so that
        // "these two runs did the same thing" is a diff of two files rather
        // than a description of two videos -- which is what the mob runtime's
        // differential control needs, and what the determinism gate has always
        // had for the core.
        //
        // AFTER the step and BEFORE the post-step callbacks, so it is the
        // state the tick produced rather than the state a script then edited:
        // a script writing through the command queue lands at the next flush,
        // and hashing on the far side of that would fold one tick's decisions
        // into the next tick's number.
        if (hash_trace && L.world.tick() % hash_trace == 0) {
            // The RNG's own state alongside the world's, because the two
            // mob runtimes share one stream and the commonest way for them to
            // part is a draw one makes and the other does not. That shows up
            // here a tick or more BEFORE it shows up in behaviour, which is
            // the difference between reading a cause and guessing at one.
            std::fprintf(stderr, "[hash] %u %u %016llX %016llX\n", li, L.world.tick(),
                         static_cast<unsigned long long>(L.world.hash()),
                         static_cast<unsigned long long>(L.ctx.rng.state()));
            // AND THE AGENTS THEMSELVES, because a hash says two runs differ
            // and never says how. Raw fixed-point, not decimals: the question
            // is exactness and a printed double would round the difference
            // being looked for.
            for (u32 a = 0; a < L.world.agent_high_water(); ++a) {
                const sim::AgentState& ag = L.world.agent(a);
                if (!ag.active) continue;
                std::fprintf(stderr, "[agent] %u %u %u %lld %lld %lld %u %u %d\n",
                             li, L.world.tick(), a,
                             static_cast<long long>(ag.pos.x.raw),
                             static_cast<long long>(ag.pos.y.raw),
                             static_cast<long long>(ag.pos.z.raw),
                             ag.facing.raw, ag.animation_state, ag.mv.sector);
            }
        }

        refresh_nav_if_stale(li);
        report_crushes(L);
        dispatch_events(L, li);

        const u64 t_lua1 = perf.every ? now_us() : 0;
        if (L.vm.is_open()) {
            // Post-step callbacks see the world the tick produced.
            L.vm.call_hook("on_post_tick", L.world.tick());
        }
        if (perf.every) perf.lua_us += now_us() - t_lua1;
        // Flush 2, before snapshots: a consequence and the state it acted on
        // ride the same snapshot rather than arriving a frame apart.
        flush_lua_commands(L);

        // And anything a script started this tick reaches the clients before
        // the snapshot that was produced with it does.
        replicate_motion_changes(li);

        if (L.world.prop_epoch() != L.prop_epoch_seen) {
            L.prop_epoch_seen = L.world.prop_epoch();
            L.props_changed_tick = L.world.tick() + 1;
        }

        if (L.vm.is_open()) {
            L.vm.end_tick(L.world.tick());
            // A fixed step per tick, so a collection cannot choose its own
            // moment, land on an arbitrary frame, push the loop past the 250 ms
            // threshold and present as a burst of client corrections.
            L.vm.gc_step();
        }
    }

    // Map changes a script asked for this tick: (level, agent) -> target level.
    //
    // Requested by agent rather than by session, because that is what a script
    // has: an entity handle. Resolved to a session here, where the mapping
    // lives.
    struct MapChange { u32 from_level; u32 agent; u32 to_level; };
    std::vector<MapChange> pending_changes;

    // --- LevelListSink --------------------------------------------------------
    //
    // The basename, not the path: a script offering a map selector wants
    // "map02", not "/home/someone/build/arm64/map02.dnpm". Computed on demand
    // rather than stored, because it is asked for once when a panel is built.
    const char* level_name(u32 i) const override {
        if (i >= levels.size()) return "";
        const std::string& p = levels[i].path;
        const size_t slash = p.find_last_of("/\\");
        const size_t start = slash == std::string::npos ? 0 : slash + 1;
        const size_t dot = p.find_last_of('.');
        const size_t len = (dot == std::string::npos || dot < start) ? std::string::npos
                                                                    : dot - start;
        levels[i].short_name = p.substr(start, len);
        return levels[i].short_name.c_str();
    }

    // --- UiSink (docs/protocol.md section 11) --------------------------------
    //
    // Applied immediately rather than queued, unlike a map change. A panel is
    // not simulation state: it is not hashed, not replicated in a snapshot, and
    // cannot make one tick's outcome depend on another's. There is nothing for
    // a queue to make deterministic.
    u32 open_ui(u32 level_index, u32 agent, const proto::UiPanel& panel) override {
        Session* s = session_for(level_index, agent);
        // No session means nobody is watching this agent -- which is the
        // ordinary case for a monster, and not an error worth logging.
        if (s == nullptr) return 0;
        if (s->open_ui.size() >= Session::MAX_OPEN_UI) {
            std::fprintf(stderr, "[srv] session %u: %zu panels already open, refusing\n",
                         s->id, s->open_ui.size());
            return 0;
        }
        proto::UiPanel p = panel;
        p.ui_id = s->next_ui_id++;
        s->open_ui.push_back(p);
        send_ui_open(*s, p);
        return p.ui_id;
    }

    void set_ui(u32 level_index, u32 agent, u32 ui_id, u16 widget_id, i32 value) override {
        Session* s = session_for(level_index, agent);
        if (s == nullptr) return;
        proto::UiPanel* p = s->find_ui(ui_id);
        if (p == nullptr) return;
        // The server's own copy is updated too, so a panel it later reasons
        // about matches what the player is looking at.
        for (u16 i = 0; i < p->count; ++i) {
            if (p->widgets[i].id != widget_id) continue;
            p->widgets[i].value = value;
            send_ui_set(*s, ui_id, widget_id, value);
            return;
        }
    }

    void set_side_texture(u32 level_index, const proto::SideTexture& t) override {
        if (level_index >= level_count()) return;
        Level& L = level(level_index);
        // One entry per (side, field): a switch thrown twice is one state, not
        // two events, and a join must not replay a history.
        bool replaced = false;
        for (auto& e : L.side_textures) {
            if (e.side == t.side && e.field == t.field) { e = t; replaced = true; break; }
        }
        if (!replaced) {
            if (L.side_textures.size() >= 512) {
                std::fprintf(stderr, "[srv] level %u: too many side textures, ignoring\n",
                             level_index);
                return;
            }
            L.side_textures.push_back(t);
        }
        for (auto& kv : sessions) {
            Session& s = kv.second;
            if (same_world(s, level_index)) send_side_texture(s, level_index, t);
        }
    }

    void send_side_texture(Session& s, u32 owner_level, const proto::SideTexture& t) {
        u8 buf[512];
        ByteWriter w(buf, sizeof(buf));
        const size_t len_pos = w.mark();
        w.u32v(0);
        w.u8v(proto::MSG_SIDE_TEXTURE);
        // The side's NAME on the wire, resolved against this level's store.
        proto::SideTexture named = t;
        named.side = level(owner_level).host->store->side_name(static_cast<i32>(t.side));
        proto::encode_side_texture(w, named);
        if (!w.ok()) return;
        const size_t end = w.size();
        const u32 body = static_cast<u32>(end - len_pos - 4);
        buf[len_pos]     = static_cast<u8>(body & 0xFF);
        buf[len_pos + 1] = static_cast<u8>((body >> 8) & 0xFF);
        buf[len_pos + 2] = static_cast<u8>((body >> 16) & 0xFF);
        buf[len_pos + 3] = static_cast<u8>((body >> 24) & 0xFF);
        link.send_control(s.id, buf, end);
    }

    void music(u32 level_index, i32 agent, const proto::MusicCue& m) override {
        if (level_index >= level_count()) return;
        // Not remembered, unlike a side texture: a cue is a moment in the
        // level's music, and a player joining later starts on the map's default
        // cue like everyone did (docs/protocol.md §11.1).
        for (auto& kv : sessions) {
            Session& s = kv.second;
            if (!s.has_agent || s.level != level_index) continue;
            if (agent >= 0 && s.agent != static_cast<u32>(agent)) continue;
            send_music(s, m);
        }
    }

    void send_music(Session& s, const proto::MusicCue& m) {
        u8 buf[512];
        ByteWriter w(buf, sizeof(buf));
        const size_t len_pos = w.mark();
        w.u32v(0);
        w.u8v(proto::MSG_MUSIC);
        proto::encode_music_cue(w, m);
        if (!w.ok()) return;
        const size_t end = w.size();
        const u32 body = static_cast<u32>(end - len_pos - 4);
        buf[len_pos]     = static_cast<u8>(body & 0xFF);
        buf[len_pos + 1] = static_cast<u8>((body >> 8) & 0xFF);
        buf[len_pos + 2] = static_cast<u8>((body >> 16) & 0xFF);
        buf[len_pos + 3] = static_cast<u8>((body >> 24) & 0xFF);
        link.send_control(s.id, buf, end);
    }

    void screen(u32 level_index, i32 agent, const proto::ScreenSet& sc) override {
        if (level_index >= level_count()) return;
        // Not remembered, for the same reason a music cue is not: a fade is a
        // moment inside a cutscene, and a player who joins halfway through one
        // should arrive at a screen they can see out of rather than at a black
        // rectangle nothing will ever lift.
        for (auto& kv : sessions) {
            Session& s = kv.second;
            if (!s.has_agent || s.level != level_index) continue;
            if (agent >= 0 && s.agent != static_cast<u32>(agent)) continue;
            send_screen(s, sc);
        }
    }

    void send_screen(Session& s, const proto::ScreenSet& sc) {
        u8 buf[256];
        ByteWriter w(buf, sizeof(buf));
        const size_t len_pos = w.mark();
        w.u32v(0);
        w.u8v(proto::MSG_SCREEN_SET);
        proto::encode_screen_set(w, sc);
        if (!w.ok()) return;
        const size_t end = w.size();
        const u32 body = static_cast<u32>(end - len_pos - 4);
        buf[len_pos]     = static_cast<u8>(body & 0xFF);
        buf[len_pos + 1] = static_cast<u8>((body >> 8) & 0xFF);
        buf[len_pos + 2] = static_cast<u8>((body >> 16) & 0xFF);
        buf[len_pos + 3] = static_cast<u8>((body >> 24) & 0xFF);
        link.send_control(s.id, buf, end);
    }

    void set_camera(u32 level_index, u32 agent, const proto::CameraSet& c) override {
        Session* s = session_for(level_index, agent);
        if (s == nullptr) return;
        send_camera(*s, c);
    }

    void set_level_camera(u32 level_index, const proto::CameraSet& c) override {
        if (level_index >= level_count()) return;
        Level& L = level(level_index);
        L.camera = c;
        L.has_camera = true;
        std::fprintf(stderr, "[srv] level %u: camera set by the map (%u field(s))\n", level_index, c.count);
        for (auto& kv : sessions) {
            Session& s = kv.second;
            if (s.has_agent && s.level == level_index) send_camera(s, c);
        }
    }

    void send_camera(Session& s, const proto::CameraSet& c) {
        u8 buf[256];
        ByteWriter w(buf, sizeof(buf));
        const size_t len_pos = w.mark();
        w.u32v(0);
        w.u8v(proto::MSG_CAMERA_SET);
        proto::encode_camera_set(w, c);
        if (!w.ok()) return;
        const size_t end = w.size();
        const u32 body = static_cast<u32>(end - len_pos - 4);
        buf[len_pos]     = static_cast<u8>(body & 0xFF);
        buf[len_pos + 1] = static_cast<u8>((body >> 8) & 0xFF);
        buf[len_pos + 2] = static_cast<u8>((body >> 16) & 0xFF);
        buf[len_pos + 3] = static_cast<u8>((body >> 24) & 0xFF);
        link.send_control(s.id, buf, end);
    }

    void close_ui(u32 level_index, u32 agent, u32 ui_id) override {
        Session* s = session_for(level_index, agent);
        if (s == nullptr) return;
        for (size_t i = 0; i < s->open_ui.size(); ++i) {
            if (s->open_ui[i].ui_id != ui_id) continue;
            s->open_ui.erase(s->open_ui.begin() + static_cast<long>(i));
            send_ui_close(*s, ui_id);
            return;
        }
    }

    // The session watching a given agent in a given level, or null.
    Session* session_for(u32 level_index, u32 agent) {
        for (auto& kv : sessions) {
            Session& s = kv.second;
            if (same_world(s, level_index) && s.agent == agent) return &s;
        }
        return nullptr;
    }

    // Called from the Lua binding. Queued, never applied here -- see tick().
    void request_map_change(u32 from_level, u32 agent, i32 to_level) override {
        if (from_level >= level_count() || agent >= sim::MAX_AGENTS) return;
        // A negative target means "the next one". The end of the last map is
        // not an error: a script asking to advance past the final level is a
        // player finishing the game, and the honest answer is to say so and
        // leave them where they are rather than to disconnect them.
        u32 target;
        if (to_level < 0) {
            target = from_level + 1;
            if (target >= level_count()) {
                std::fprintf(stderr, "[srv] agent %u finished the last level (%s)\n",
                             agent, levels[from_level].path.c_str());
                return;
            }
        } else {
            target = static_cast<u32>(to_level);
            if (target >= level_count()) {
                std::fprintf(stderr, "[srv] agent %u asked for level %d; only %u exist\n",
                             agent, to_level, level_count());
                return;
            }
        }
        if (target == from_level) return;
        // One per agent per tick. A script that fires an exit twice -- two
        // switches on the same wall, a trigger crossed and used -- must move
        // the player once.
        for (const MapChange& c : pending_changes) {
            if (c.from_level == from_level && c.agent == agent) return;
        }
        pending_changes.push_back(MapChange{from_level, agent, target});
    }

    // WHERE A LEVEL'S OWN COORDINATES SIT IN THE WORLD.
    //
    // Zero for a level off the grid and zero for every level when the metamap
    // declares no pitch, which is the same answer and means the same thing:
    // this map's coordinates ARE world coordinates, and there is no seam to
    // cross because there is nothing beside it.
    fx2 origin_of(u32 li) const {
        if (li >= levels.size()) return fx2{};
        return server::world::cell_origin(levels[li].cell, meta.pitch_x, meta.pitch_y);
    }

    // THE BODY WALKED INTO ANOTHER LEVEL OF THE REGION, and that is all.
    //
    // Same world, same agent, same input ring, same snapshot history -- none
    // of what a WELCOME resets. What changes is the level it is IN: whose
    // music and camera apply, what every held level's offset is measured
    // from, which level counts as occupied. So the session is re-homed, told
    // with a HOME message, and its neighbourhood re-stated from there.
    //
    // This replaced the seam CROSSING, which moved the body into a different
    // simulation at every cell boundary -- a reseat, a new predictor on the
    // client, and every bug that came with that.
    void update_region_homes() {
        if (!region_) return;
        bool moved = false;
        for (auto& [id, s] : sessions) {
            if (!s.has_agent || !in_region(s.level)) continue;
            const sim::AgentState& a = region_->world.agent(s.agent);
            if (!a.active || a.mv.sector < 0) continue;
            const u32 now = region_->store->chunk_of_sector(static_cast<u32>(a.mv.sector));
            if (now == geom::NO_CHUNK || now == s.level || !is_live(now)) continue;
            const u32 was = s.level;
            s.level = now;
            // The level left becomes held like any other; the one entered was
            // held already (it is next door), so no map follows.
            if (!server::world::rehome(s.held, s.has_home, was, now)) {
                std::fprintf(stderr, "[srv] session %u walked into level %u, which it did "
                             "not hold\n", id, now);
                send_map(level(now), id);
            }
            send_home(id, now);
            sync_neighbours(s, id);
            Level& L = level(now);
            if (L.has_camera) send_camera(s, L.camera);
            std::fprintf(stderr, "[srv] session %u walked from level %u into level %u\n",
                         id, was, now);
            moved = true;
        }
        // Who is where decides what is resident.
        if (moved) refresh_residency();
    }

    void send_home(u32 id, u32 li) {
        u8 msg[16];
        ByteWriter w(msg, sizeof(msg));
        const size_t len_pos = w.mark();
        w.u32v(0);
        w.u8v(proto::MSG_HOME);
        w.u16v(static_cast<u16>(li));
        finish_frame(msg, len_pos, w.size());
        link.send_control(id, msg, w.size());
    }

    void apply_map_changes() {
        if (pending_changes.empty()) return;
        for (const MapChange& c : pending_changes) {
            for (auto& [id, s] : sessions) {
                if (!same_world(s, c.from_level) || s.agent != c.agent) continue;
                const std::string from = levels[c.from_level].path;
                if (!place_in_level(s, id, c.to_level)) {
                    std::fprintf(stderr, "[srv] session %u cannot enter level %u (full)\n",
                                 id, c.to_level);
                    break;
                }
                std::fprintf(stderr, "[srv] session %u: %s -> %s (agent %u)\n",
                             id, from.c_str(), levels[c.to_level].path.c_str(), s.agent);
                break;
            }
        }
        pending_changes.clear();
        // A player walked through an exit, so the neighbourhood moved with
        // them: what they left may now be droppable and what they walked
        // towards is already in memory, because it was a neighbour before it
        // was a destination. Here rather than per tick -- the answer is a
        // function of who is where, and that does not change sixty times a
        // second.
        refresh_residency();
    }

    /**
     * Is anybody in this level?
     *
     * By SESSION and not by agent: a session that has arrived but has not been
     * given an agent yet is still somebody waiting for this level to run, and
     * skipping it would be a level that never spawns them.
     */
    // A stationary body of PLAYER kind on level 0, and the level ticked from
    // tick 0 because of it.
    //
    // FOR COMPARING TWO RUNS, and it exists because that is otherwise
    // impossible. A level is not ticked until a session arrives, its clock
    // keeps counting while it waits (step_idle, deliberately -- sector motion
    // is a function of the tick), and so the tick a player lands on depends on
    // wall clock. Mobs think on `tick % 8`, so two runs of the same scenario
    // start with a different set of monsters deciding first and never agree
    // again -- not by a shift, which could be re-based, but by phase.
    //
    // NOT FOR PLAY. It puts a body in the world that nobody controls and that
    // no client is told about.
    bool test_player = false;

    bool level_occupied(u32 li) const {
        if (test_player && li == 0) return true;
        for (const auto& [id, s] : sessions) if (s.level == li) return true;
        return false;
    }

    void tick() {
        // ONE COUNT FOR THE WHOLE SERVER, advanced exactly where a level's own
        // world advances: World::step and World::step_idle each add one, and
        // every resident level runs one of the two below. A level loaded later
        // is set to this, which is what keeps them all saying the same number.
        ++clock_;

        // EMPTY LEVELS DO NOT TICK.
        //
        // This reverses what the comment on tick_level argued for, and the
        // reason is a measurement rather than a preference. `--module doom2`
        // hosts thirty-two levels and about 3,800 monsters; running every one
        // of them every tick while a player stands in exactly one cost the
        // server its deadline -- repeated "fell 250 ms behind" and a game that
        // was not playable. The comment predicted this ("the moment it is not
        // [cheap]"), and preferred making an idle level cheap to making time
        // conditional. But nearly all of an idle level's cost IS its monsters'
        // AI and physics, so "cheap" and "not ticking" differ only in whether
        // the lifts keep moving -- which nobody is there to see.
        //
        // WHAT THIS CHANGES, stated plainly: a level's clock stops while it is
        // empty, so a lift left mid-travel is still mid-travel when somebody
        // arrives, rather than wherever it would have got to. That is coherent
        // because every clock here is already per-level -- there is no global
        // tick, and parametric sector motion is a function of the LEVEL's tick
        // (docs/protocol.md), so a client joining computes exactly the position
        // the server has. It is also what Doom itself does with a level you are
        // not in.
        //
        // Arrivals are safe: apply_map_changes() runs AFTER this loop, so a
        // player transferred into an empty level is already in `sessions` when
        // the next tick asks who is where.
        u32 ran = 0;
        for (u32 li = 0; li < level_count(); ++li) {
            // NOT LOADED IS NOT IDLE. An idle level keeps its clock because a
            // client walking into a stopped one jumps its own backwards; an
            // UNLOADED level has no clock to keep, and its slot holds the tick
            // it stopped at until somebody comes near enough to want it back.
            if (!is_live(li)) continue;
            // The region's levels tick together, below.
            if (in_region(li)) continue;
            if (!level_occupied(li)) {
                // The clock and the map, and nothing else. NOT a skipped tick:
                // every level keeps the same count, because a level whose clock
                // stops is thousands of ticks behind by the time somebody walks
                // into it, and a client crossing that gap jumps its clock
                // backwards far enough that the server's input ring seeds
                // itself from a straggler and discards everything real after
                // it. That was reported as "player inputs stop working after
                // transitioning to a map", and it is why this is step_idle()
                // rather than `continue`.
                level(li).world.step_idle();
                ++idle_levels_skipped_;
                continue;
            }
            tick_level(li);
            ++ran;
        }
        // Said once, the first time it happens, because it is a real change in
        // what the server is doing and it should not be folklore.
        if (!idle_skip_reported_ && ran < level_count()) {
            idle_skip_reported_ = true;
            u32 live = 0;
            for (u32 li = 0; li < level_count(); ++li) if (is_live(li)) ++live;
            std::fprintf(stderr,
                "[srv] %u of %u levels are empty and are not being ticked; "
                "%u of %u are in memory at all. An empty level's clock keeps "
                "count; an unloaded one keeps the tick it stopped at\n",
                level_count() - ran, level_count(), live, level_count());
        }
        // AFTER every level has stepped, never during one.
        //
        // A map change despawns from one world and spawns in another, and doing
        // that while a world is mid-step would mutate the entity set a tick is
        // iterating. It also keeps the rule the command queue already follows:
        // scripts ask, and the ask lands at a defined point.
        if (region_ && region_live_count() > 0) tick_region();
        apply_map_changes();
        update_region_homes();
    }

    // WHICH OF THE REGION'S LEVELS ARE AWAKE this tick: one a player is in, or
    // within a quarter of a cell of. Only those run their scripts, and only
    // their bodies are stepped -- what an empty level not ticking used to buy,
    // said per level of one world. Walking up to a boundary wakes the level
    // beyond it before you reach it.
    std::vector<u8> region_awake_;
    void compute_region_awake() {
        region_awake_.assign(levels.size(), 0);
        const Host& H = *region_;
        const fx mx = fx::from_raw(meta.pitch_x.raw / 4);
        const fx my = fx::from_raw(meta.pitch_y.raw / 4);
        const auto wake_near = [&](fx3 p) {
            for (u32 li = 0; li < levels.size(); ++li) {
                if (!in_region(li) || !levels[li].live) continue;
                const fx2 o = origin_of(li);
                if (p.x >= o.x - mx && p.x <= o.x + meta.pitch_x + mx &&
                    p.y >= o.y - my && p.y <= o.y + meta.pitch_y + my) {
                    region_awake_[li] = 1;
                }
            }
        };
        for (const auto& [id, s] : sessions) {
            if (!s.has_agent || !in_region(s.level)) continue;
            wake_near(H.world.agent(s.agent).pos);
        }
        if (test_player && in_region(0) && levels[0].live) region_awake_[0] = 1;
    }

    // ONE TICK OF THE REGION: every level's script around one step of one world.
    void tick_region() {
        Host& H = *region_;
        compute_region_awake();
        u32 first = NO_OWNER;
        for (u32 li = 0; li < levels.size(); ++li) {
            if (in_region(li) && levels[li].live) { first = li; break; }
        }

        // A body is asleep when the level that spawned it is. Players are
        // nobody's, so never.
        for (u32 a = 0; a < H.world.agent_high_water(); ++a) {
            sim::AgentState& ag = H.world.agent_mut(a);
            if (!ag.active) continue;
            const u32 o = H.owner[a];
            ag.asleep = o != NO_OWNER && o < levels.size() && !region_awake_[o];
        }

        for (auto& [id, s] : sessions) {
            if (s.has_agent && in_region(s.level)) H.world.set_input(s.agent, s.take());
        }

        bool any = false;
        for (u32 li = 0; li < levels.size(); ++li) {
            if (!in_region(li) || !levels[li].live || !region_awake_[li]) continue;
            any = true;
            Level& L = level(li);
            const u64 t_lua0 = perf.every ? now_us() : 0;
            if (L.vm.is_open()) {
                L.vm.begin_tick();
                L.vm.call_hook("on_tick", H.world.tick());
            }
            if (perf.every) perf.lua_us += now_us() - t_lua0;
            drive_param_test(li);
            flush_lua_commands(L);
        }
        refresh_effect_params(first);

        if (!any) {
            // Nobody near anything: the clock and the map, as an empty level.
            H.world.step_idle();
            ++idle_levels_skipped_;
            return;
        }
        const u64 t_step0 = perf.every ? now_us() : 0;
        H.world.step();
        if (perf.every) {
            perf.step_us += now_us() - t_step0;
            perf.bp_full += H.world.broadphase_full();
        }
        refresh_nav_if_stale(first);
        report_crushes(level(first));
        for (u32 li = 0; li < levels.size(); ++li) {
            if (!in_region(li) || !levels[li].live || !region_awake_[li]) continue;
            Level& L = level(li);
            dispatch_events(L, li);
            const u64 t_lua1 = perf.every ? now_us() : 0;
            if (L.vm.is_open()) L.vm.call_hook("on_post_tick", H.world.tick());
            if (perf.every) perf.lua_us += now_us() - t_lua1;
            flush_lua_commands(L);
        }
        replicate_motion_changes(first);
        if (H.world.prop_epoch() != H.prop_epoch_seen) {
            H.prop_epoch_seen = H.world.prop_epoch();
            H.props_changed_tick = H.world.tick() + 1;
        }
        for (u32 li = 0; li < levels.size(); ++li) {
            if (!in_region(li) || !levels[li].live || !region_awake_[li]) continue;
            Level& L = level(li);
            if (L.vm.is_open()) {
                L.vm.end_tick(H.world.tick());
                L.vm.gc_step();
            }
        }
    }

    // Transients are cleared AFTER sending, not once a tick.
    //
    // Snapshots go out on a divisor -- 60 Hz simulation, 30 Hz snapshots -- so a
    // queue emptied every tick threw away everything queued on the ticks in
    // between. Half the tracers vanished, and the ones that survived did so by
    // landing on an even tick.
    //
    // Cleared at the end of this function instead: the queue holds whatever
    // accumulated since the last send, and is emptied once it has gone out.
    void send_snapshots() {
        // Once per level, not once per session: the offsets are a property of
        // the world, and every observer of a moving sector sees the same one.
        for (u32 li = 0; li < level_count(); ++li) {
            if (is_live(li)) collect_discrete_offsets(level(li));
        }

        for (auto& [id, s] : sessions) {
            if (!s.has_agent) continue;
            // Everything below is read from the sender's OWN level. A snapshot
            // is a description of one world, and the entity indices, sector
            // indices and tick in it mean nothing anywhere else.
            Level& L = level(s.level);
            const sim::World& world = L.world;

            const sim::AgentState& self = world.agent(s.agent);

            // Delta against whatever the client last confirmed it applied. If
            // that snapshot has aged out of the ring -- or the client has never
            // acked -- fall back to a full snapshot, which is always correct.
            const SnapshotRecord* baseline = s.history.find(s.ack_snapshot_tick);

            proto::SnapshotHeader h;
            h.server_tick     = world.tick();
            h.baseline_tick   = baseline ? baseline->tick : 0;
            h.last_input_tick = s.last_applied_tick;
            h.input_margin    = s.margin();
            h.flags           = 0;
            // Per observer: only if a script touched THIS agent since the last
            // snapshot this session received (docs/protocol.md §10.1.3).
            //
            // A GEOMETRY change counts too, and is not per-agent: a script that
            // opened a door changed the world every observer of that door
            // predicts against, and none of them could have known. That is one
            // tick of mismatch that self-heals the moment the motion message
            // lands -- the same shape as a scripted teleport, and it deserves
            // the same label rather than being reported as a defect.
            //
            // Only on the ticks a motion actually CHANGED, never on every tick
            // a script ran: a flag set unconditionally would excuse every
            // client's mismatch forever and destroy the signal it exists for.
            if (L.lua_touched_tick[s.agent] > s.last_snapshot_sent ||
                L.motion_changed_tick > s.last_snapshot_sent) {
                h.flags |= proto::SNAP_LUA_APPLIED;
            }

            // The whole controller state, not just position. Reconciliation
            // overwrites the client's copy and replays from it, so anything
            // omitted here would survive the overwrite and diverge silently.
            proto::SelfBlock sb;
            sb.pos             = self.pos;
            sb.vel             = self.vel;
            sb.facing          = self.facing;
            sb.controller_mode = static_cast<u8>(self.mv.mode);
            sb.state_flags     = world.state_flags(s.agent);
            sb.jumps_used      = self.mv.jumps_used;
            sb.dashes_used     = self.mv.dashes_used;
            sb.attack_left     = self.mv.attack_left;
            sb.sector          = world.sector_name(self.mv.sector);   // by name
            sb.dash_timer      = self.mv.dash_timer;
            sb.dash_cooldown   = self.mv.dash_cooldown_timer;
            sb.hover_fuel      = self.mv.hover_fuel;
            sb.flight_fuel     = self.mv.flight_fuel;
            sb.grapple_anchor  = self.mv.grapple_anchor;
            sb.grapple_dist    = self.mv.grapple_dist;
            // The sector this entity rides (design §3.2.5), encoded with the
            // same "raw 0 means none" convention handles use. Not replicating
            // it left the client carrying a stale anchor across any server-side
            // move -- see docs/protocol.md §5.1.
            sb.anchor          = proto::Handle{self.mv.anchor >= 0
                                     ? static_cast<u32>(world.sector_name(self.mv.anchor)) + 1u : 0u};
            // The observer's own animation state. Every other body's reaches
            // its watchers through an EntityBlock; this one is excluded from
            // its own stream (see the `i == s.agent` skip below), so without
            // this line a script animating a player animates nobody's screen.
            sb.avatar          = L.avatar[s.agent];
            sb.animation_state = world.agent(s.agent).animation_state;
            sb.param_epoch     = L.param_epoch[s.agent];
            sb.state_hash      = world.agent_hash(s.agent);

            u8 buf[proto::MAX_DATAGRAM_BYTES];
            proto::SnapshotWriter w(buf, sizeof(buf));
            w.begin(h, sb, L.hp[s.agent], L.discrete_offsets, L.discrete_count);

            SnapshotRecord& record = s.history.begin(world.tick());

            u32 omitted = 0;
            u32 unchanged = 0;
            // ONE loop over the whole entity space. Agents occupy the low
            // slots and props the rest, which is why they need no separate
            // message, no handle domain bit, and no second delta path -- from
            // the wire's point of view a key and a player differ by what they
            // are called and how often they move.
            // WHAT GETS DROPPED WHEN THE DATAGRAM IS FULL.
            //
            // This used to sweep 0..MAX_ENTITIES and omit everything after the
            // index it ran out of room on. Agents hold the low indices and
            // props the high ones, so a level with two hundred monsters spent
            // the entire budget on agents and never sent a single prop -- the
            // keys did not appear, and nothing was wrong with them: they exist
            // on the server and no client was ever told.
            //
            // Order the sweep instead. NEAR entities first, every tick, so
            // whatever the player can actually see is current; then the rest
            // from a per-session cursor that advances, so the far field takes
            // turns instead of the same tail starving forever.
            //
            // Ordering, not filtering: nothing is excluded, and an entity that
            // does not fit this tick is still recorded as "the client has the
            // old value" so the next delta is computed against what arrived.
            // SORTED BY DISTANCE, which is what docs/protocol.md section 5.4
            // has always specified and what this did not do.
            //
            // What was here was a binary near/far split at a radius of 64
            // engine units -- 3072 Doom map units, which is most of a level --
            // and WITHIN each half the order was the entity INDEX. Two things
            // followed, and both are worse than they sound:
            //
            //   the fifty entities a player was told about were the fifty with
            //   the lowest slot numbers, not the fifty nearest. Slot order is
            //   spawn order, so it was "whatever the map script placed first",
            //   for ever, however far away it had wandered;
            //
            //   and when the near half alone overflowed the budget -- which on
            //   any real Doom level it does -- the far half was never reached
            //   at all. The cursor advanced and nothing read it. The comment
            //   below it promised the far field "takes turns"; it did not.
            //
            // Only ACTIVE entities are candidates now. The old sweep walked all
            // 1152 slots twice, reading positions out of dead ones, which is
            // also why `order` had to be the size of the whole table.
            u32 order[sim::MAX_ENTITIES];
            u32 order_n = 0;
            {
                const fx3 eye = world.agent(s.agent).pos;
                const auto pos_of = [&](u32 i) {
                    return sim::World::is_prop(i) ? world.prop(i).pos
                                                  : world.agent(i).pos;
                };
                const auto live = [&](u32 i) {
                    return sim::World::is_prop(i) ? world.prop(i).active
                                                  : world.agent(i).active;
                };

                // The key is the horizontal distance squared, in raw fx units,
                // divided down as the entity goes unsent.
                //
                // "Distance, then time since last sent" is the spec's rule, and
                // a strict sort on distance alone starves the far field exactly
                // as the old code did -- an entity beyond the budget is never
                // sent, so it is never not-stale, so it never rises. Halving
                // its effective distance every AGE_TICKS unsent snapshots gives
                // it a way in without letting it outrank something actually in
                // front of the player: a body twice as far away has to go four
                // times as long unheard before it competes.
                constexpr u32 AGE_TICKS = 8;
                constexpr u32 AGE_MAX_SHIFT = 12;
                struct Cand { u64 key; u32 i; };
                Cand cand[sim::MAX_ENTITIES];

                for (u32 i = 0; i < sim::MAX_ENTITIES; ++i) {
                    if (i == s.agent) continue;

                    // A DEAD SLOT THE CLIENT STILL BELIEVES IN IS A CANDIDATE,
                    // and leaving it out was a real regression: the only thing
                    // that sends DESTROYED is the loop below noticing an entity
                    // that is no longer present while the baseline says it was.
                    // Filtering to live entities meant a picked-up key stayed on
                    // the floor of every client for ever -- caught by
                    // doors.test.js's "walking into it destroyed it".
                    const bool believed = baseline && baseline->entities[i].present;
                    if (!live(i) && !believed) continue;

                    if (!live(i)) {
                        // Top priority, and it costs six bytes. A destruction is
                        // the one message that does not become truer by waiting,
                        // and an entity the client draws that the server does
                        // not have is the most confusing thing a snapshot can
                        // leave behind.
                        cand[order_n++] = Cand{0, i};
                        continue;
                    }

                    const fx3 p = pos_of(i);
                    const fx2 d{p.x - eye.x, p.y - eye.y};
                    u64 key = static_cast<u64>(
                        len_sq_raw(d) >> 32);   // fx raw squared is wide; 32 bits of it is plenty
                    // +1 so nothing that is merely close outranks a destruction.
                    key += 1;
                    const u32 unsent = s.last_sent_tick[i] == 0
                                     ? AGE_MAX_SHIFT * AGE_TICKS
                                     : world.tick() - s.last_sent_tick[i];
                    const u32 shift = unsent / AGE_TICKS;
                    key >>= (shift > AGE_MAX_SHIFT ? AGE_MAX_SHIFT : shift);
                    cand[order_n++] = Cand{key, i};
                }

                // Insertion order is the tie-break, and it is the entity index,
                // so two bodies at the same distance are still ordered the same
                // way on every tick -- a snapshot set that reshuffled under a
                // tie would make the delta baselines churn for no reason.
                std::stable_sort(cand, cand + order_n,
                                 [](const Cand& a, const Cand& b) { return a.key < b.key; });
                for (u32 k = 0; k < order_n; ++k) order[k] = cand[k].i;
            }

            // ROOM KEPT BACK FOR THE TRANSIENTS, and this is why it exists.
            //
            // The wire's order is entities then events, so the events are
            // written last -- and on a populated level the entities will use
            // every byte. Doom's MAP01 alone places thirty-one monsters and
            // thirty-one props against a datagram that holds about forty-six
            // entity blocks, so there was nothing left and every tracer,
            // every spray of blood and every explosion was dropped. It looks
            // exactly like the effect not working, and it is the effect never
            // being sent.
            //
            // The two do not fail alike, which is what decides who yields. An
            // entity that does not fit is re-sent on the next snapshot and the
            // client carries the old one forward meanwhile -- that is the whole
            // delta design, and the loop above already handles it. An event
            // that does not fit is GONE: it happened at a moment that has
            // passed, and the comment on the transient loop below says so.
            //
            // So the recoverable thing yields to the unrecoverable one, but
            // only by a bounded amount: eight events is 178 bytes of 1200, or
            // about seven entities' worth, and anything past eight still takes
            // its chances on what the entities leave.
            // ONE EVENT OF SLACK, because the loop stops when the budget is
            // AT MOST the reserve and not exactly it: the last entity written
            // can leave a few bytes under, and without the slack that is the
            // difference between eight events fitting and seven.
            const u32 pending_vfx = world.vfx_count();
            const u32 want_vfx = pending_vfx < 8 ? pending_vfx : 8;
            const size_t vfx_reserve =
                pending_vfx == 0 ? 0
                                 : (want_vfx + 1) * proto::SnapshotWriter::VFX_BYTES + 2;

            for (u32 oi = 0; oi < order_n; ++oi) {
                // Stop ADDING entities once the reserve is all that is left.
                // Not a `break`: the loop below still has to record what the
                // client actually holds for every remaining entity, or the next
                // delta is computed against data that never arrived.
                const u32 i = order[oi];
                if (vfx_reserve > 0 && i != s.agent && w.remaining() <= vfx_reserve) {
                    // What the client STILL holds, exactly as the "did not fit"
                    // path below records it -- anything else and the next delta
                    // is computed against data that never arrived.
                    record.entities[i] = baseline ? baseline->entities[i] : EntitySample{};
                    ++omitted;
                    continue;
                }
                // The observer's own agent is in SelfBlock at full precision.
                // Sending it again here would let the client reconcile against
                // the quantised copy of itself.
                if (i == s.agent) continue;

                const EntitySample was = baseline ? baseline->entities[i] : EntitySample{};

                EntitySample now;
                now.generation = world.generation(i);
                if (sim::World::is_prop(i)) {
                    const sim::PropState& p = world.prop(i);
                    now.present         = p.active;
                    now.pos             = p.pos;
                    now.facing          = p.facing;
                    now.animation_state = p.animation_state;
                    // The kind travels in display_id, which the protocol
                    // reserved for exactly this: a number naming what a thing
                    // IS, that the client turns into something to draw. It is
                    // sent once, on the tick the entity appears, because a key
                    // does not become a different key.
                    now.display_id      = p.kind;
                } else {
                    const sim::AgentState& a = world.agent(i);
                    now.present         = a.active;
                    now.pos             = a.pos;
                    now.facing          = a.facing;
                    // An enemy is an agent with a kind, so it replicates
                    // through exactly the path a player does and is drawn from
                    // exactly the records a prop is. Zero stays "a player".
                    now.animation_state = a.animation_state;
                    now.display_id      = a.kind;
                    // Only a player has an avatar; an enemy is drawn from its
                    // kind. Reading it for both would be harmless and would also
                    // suggest a monster could wear one.
                    now.avatar          = a.kind == 0 ? L.avatar[i] : 0;
                    now.solid           = a.solid;
                }

                if (!now.present) {
                    // Only tell the client about a destruction it has not
                    // already been told about.
                    if (was.present) {
                        proto::EntityBlock e;
                        e.handle = proto::Handle::make(i + 1, was.generation);
                        e.mask   = proto::ent_field::DESTROYED;
                        if (w.try_entity(e)) {
                            record.entities[i] = EntitySample{};   // client now agrees it is gone
                        } else {
                            record.entities[i] = was;              // retry next snapshot
                            ++omitted;
                        }
                    }
                    continue;
                }

                const u32 mask = delta_mask(was, now);
                if (mask == 0) {
                    // Unchanged: omit entirely. The client carries it forward
                    // from the baseline.
                    ++unchanged;
                    record.entities[i] = now;
                    continue;
                }

                proto::EntityBlock e;
                e.handle           = proto::Handle::make(i + 1, now.generation);
                e.mask             = mask;
                e.pos              = now.pos;
                e.facing           = now.facing;
                e.animation_state  = now.animation_state;
                e.display_id       = now.display_id;
                e.avatar           = now.avatar;
                e.solid            = now.solid ? 1 : 0;

                if (w.try_entity(e)) {
                    record.entities[i] = now;
                    // WHEN, so the ordering above can age it. `tick` rather
                    // than a counter of snapshots: a client that joins late
                    // must not look like one whose entities are all fresh.
                    s.last_sent_tick[i] = world.tick();
                } else {
                    // Did not fit. Record what the client STILL has, not what we
                    // wanted to send -- otherwise the next delta is computed
                    // against data that never arrived and the entity freezes at
                    // a stale position with nothing reporting an error.
                    record.entities[i] = was;
                    ++omitted;
                }
            }

            // Transients, after the entities. Fire and forget: whatever does
            // not fit is dropped rather than deferred, because a tracer that
            // arrived a snapshot late would be drawn at a moment that has
            // already passed. This is the first thing to send them -- the
            // encoder has existed and been unit-tested since Phase 1 and had
            // never been called.
            u32 vfx_dropped = 0;
            for (u32 v = 0; v < world.vfx_count(); ++v) {
                const sim::PendingVfx& pv = world.vfx(v);
                proto::VfxEvent ev;
                ev.kind = pv.kind;
                ev.pos = pv.pos;
                ev.dir = pv.dir;
                ev.param = pv.param;
                if (!w.try_vfx(ev)) { vfx_dropped = world.vfx_count() - v; break; }
            }

            s.last_snapshot_sent = world.tick();
            const size_t n = w.finish();
            if (omitted > 0) {
                // Protocol R10: never silently cap. But said once every ten
                // seconds, not per snapshot: a region's world holds every
                // resident level's monsters, the budget is exceeded on every
                // datagram by design (nearest first), and a line thirty times
                // a second is a log nobody can read anything else in.
                s.omitted_since_report += omitted;
                if (s.omitted_reported_tick == 0 ||
                    world.tick() - s.omitted_reported_tick >= 10 * sim::TICK_RATE_HZ) {
                    std::fprintf(stderr, "[srv] tick %u session %u: %u entities omitted "
                                 "(budget) in this snapshot, %llu since the last report\n",
                                 world.tick(), id, omitted,
                                 static_cast<unsigned long long>(s.omitted_since_report));
                    s.omitted_reported_tick = world.tick();
                    s.omitted_since_report = 0;
                }
            }
            if (vfx_dropped > 0) {
                // Same rule, and it had been broken here: transients are sent
                // AFTER the entities, so on a crowded map the entity list fills
                // the datagram and every tracer is discarded -- silently, since
                // `omitted` counts only entities. Twenty-nine monsters is
                // enough to do it, and the symptom is "the hitscan enemies
                // never draw anything", with nothing in any log.
                std::fprintf(stderr, "[srv] tick %u session %u: %u vfx dropped (budget)\n",
                             world.tick(), id, vfx_dropped);
            }
            s.bytes_sent += n;
            s.snapshots_sent += 1;
            s.entities_omitted += unchanged;
            if (n > 0) link.send_datagram(id, buf, n);
        }
    
        // Sent; the queue may go.
        for (u32 li = 0; li < level_count(); ++li) {
            if (is_live(li)) level(li).world.clear_vfx();
        }
}
};

}  // namespace

#if defined(__ANDROID__)
#include <malloc.h>
// ANDROID TAGS ITS HEAP POINTERS, AND LUAJIT CANNOT SEE PAST THE TAG.
//
// From Android 11, bionic puts a tag in the TOP BYTE of every pointer malloc
// returns -- 0xb40079c5ec048020 rather than 0x0079c5ec048020. The hardware
// ignores that byte when it loads and stores (Top Byte Ignore), so the pointer
// works perfectly for C. It does not work for LuaJIT: a 64-bit LuaJIT packs a
// pointer into 47 bits and checks up front that it fits, and 0xb4 up there does
// not. lua_newstate returns NULL.
//
// What the server then printed was "lua_newstate failed (memory cap too low?)",
// which is an honest guess and the wrong one, and it cost an hour. The guess is
// still the right thing to print on every other platform.
//
// So tagging is turned off before anything allocates. It is a per-process
// setting that only affects LATER allocations, which is why this is the first
// statement in main() and not somewhere tidier. The manifest's
// allowNativeHeapPointerTagging="false" covers the APP process; this one is a
// child of it, exec'd rather than forked from the zygote, so it gets the
// platform default and has to opt out for itself.
//
// WHAT IS GIVEN UP: Scudo's use-after-free detection on this process. Worth
// saying plainly rather than leaving as a footnote -- and the trade is not
// close, because the alternative is no scripting at all.
static void dnp_untag_heap() {
    if (mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, M_HEAP_TAGGING_LEVEL_NONE) != 1) {
        std::fprintf(stderr, "[srv] could not turn off heap pointer tagging; "
                             "Lua will refuse to start\n");
    }
}
#else
static void dnp_untag_heap() {}
#endif

int main(int argc, char** argv) {
    dnp_untag_heap();

    u16 port = 34500;
    // Repeatable, and the order IS the progression: `--map a --map b` means an
    // exit in a takes you to b.
    constexpr u32 MAX_LEVELS = 64;
    const char* map_paths[MAX_LEVELS]{};
    const char* metamap_path = nullptr;
    u32 map_count = 0;
    const char* spawn_tag_arg = nullptr;
    bool want_param_test = false;
    constexpr u32 MAX_SCRIPTS = 8;
    const char* script_args[MAX_SCRIPTS]{};
    u32 script_count = 0;
    bool strict_scripts = false;
    bool no_scripts = false;

    // ALSO FROM THE ENVIRONMENT, and that is the point of them.
    //
    // The things that most need measuring are launched by something else: the
    // edge tests spawn the server with their own argument list, run_stack.sh
    // builds one, and the Android shell builds one in Java. Making these
    // flags-only would mean editing each of those to ask a question, and the
    // APK is the one where that is hardest and the answer matters most.
    //
    // A flag still wins, so a command line remains the last word.
    u32 hash_trace_every = 0;
    bool test_player = false;
    const char* env_perf = std::getenv("DNP_PERF");
    const char* env_stall = std::getenv("DNP_LUA_STALL_US");
    u32  perf_every = env_perf ? static_cast<u32>(std::atoi(env_perf)) : 0;
    u32  lua_stall_us = env_stall ? static_cast<u32>(std::atoi(env_stall)) : 0;
    const char* props_path = nullptr;      // --props wins over any module
    constexpr u32 MAX_MODULES = 16;
    const char* module_dirs[MAX_MODULES]{};
    u32 module_count = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = static_cast<u16>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            if (map_count < MAX_LEVELS) map_paths[map_count++] = argv[++i];
            else { std::fprintf(stderr, "[srv] too many --map arguments\n"); return 2; }
        } else if (std::strcmp(argv[i], "--metamap") == 0 && i + 1 < argc) {
            metamap_path = argv[++i];
        } else if (std::strcmp(argv[i], "--map-chunk") == 0 && i + 1 < argc) {
            const long n = std::atol(argv[++i]);
            MAP_CHUNK_BYTES = n < 64 ? 64 : static_cast<size_t>(n);
        } else if (std::strcmp(argv[i], "--spawn") == 0 && i + 1 < argc) {
            spawn_tag_arg = argv[++i];
        } else if (std::strcmp(argv[i], "--param-test") == 0) {
            want_param_test = true;
        } else if (std::strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
            // Repeatable, and loaded in the order given. A converted Doom map
            // is two files -- the shared action library and the map's own data
            // -- and the alternative is either `require` (absent under strict
            // trust) or a copy of the library inlined into all 32 of them.
            if (script_count < MAX_SCRIPTS) script_args[script_count++] = argv[++i];
            else { std::fprintf(stderr, "[srv] too many --script arguments\n"); return 2; }
        } else if (std::strcmp(argv[i], "--props") == 0 && i + 1 < argc) {
            props_path = argv[++i];
        } else if (std::strcmp(argv[i], "--module") == 0 && i + 1 < argc) {
            // A project's own maps, scripts and assets in one directory (see
            // docs/MODULES.md). REPEATABLE: several modules load together, and
            // their catalogues are concatenated rather than renumbered, so an
            // id means the same thing to the server and to the client.
            if (module_count < MAX_MODULES) module_dirs[module_count++] = argv[++i];
            else { std::fprintf(stderr, "[srv] too many --module arguments\n"); return 2; }
        } else if (std::strcmp(argv[i], "--no-scripts") == 0) {
            // Refuse to run any Lua at all, including what the maps ask for.
            // The operator's veto, and the control every "the map is driving
            // this" test needs.
            no_scripts = true;
        } else if (std::strcmp(argv[i], "--script-trust") == 0 && i + 1 < argc) {
            strict_scripts = std::strcmp(argv[++i], "strict") == 0;
        } else if (std::strcmp(argv[i], "--perf") == 0 && i + 1 < argc) {
            // Report where each tick went, every N ticks. See Server::Perf.
            perf_every = static_cast<u32>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--test-player") == 0) {
            // A stationary player-kind body at the spawn tag, and level 0
            // ticked from tick 0. See Server::test_player.
            test_player = true;
        } else if (std::strcmp(argv[i], "--hash-trace") == 0 && i + 1 < argc) {
            // One line per level per N ticks: the world's own state hash. For
            // comparing two runs of the same scenario against each other.
            hash_trace_every = static_cast<u32>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--lua-stall-us") == 0 && i + 1 < argc) {
            // The stall detector's threshold, which was a compile-time constant.
            //
            // Measuring the ordinary per-tick Lua cost meant editing vm.hpp and
            // rebuilding, because the detector only prints above the threshold
            // and the default is 4000 us -- a level well inside budget reports
            // nothing at all, which is correct for an operator and useless for
            // anybody asking "how much does a monster cost".
            lua_stall_us = static_cast<u32>(std::atoi(argv[++i]));
        }
    }

    Server srv;
    srv.perf.every = perf_every;
    srv.param_test = want_param_test;
    if (spawn_tag_arg) {
        srv.spawn_tag = spawn_tag_arg;
        srv.spawn_tag_len = static_cast<u32>(std::strlen(spawn_tag_arg));
    }
    // A MODULE BRINGS ITS OWN MAPS.
    //
    // `--module doom2` with no --map loads everything in modules/doom2/maps,
    // in name order, as a progression -- which for a converted game is exactly
    // map01 through map32 and an exit that advances. Naming thirty-two files on
    // a command line to play a game that ships thirty-two of them is a chore
    // the module already knows the answer to.
    //
    // DISCOVERED, not declared. module.txt deliberately does not list contents:
    // a manifest that has to be kept in step with the filesystem is a second
    // source of truth, and it goes stale the first time somebody adds a map.
    //
    // An explicit --map still wins. Order matters and a module whose maps do
    // not sort into the order they should be played in has to say so, which is
    // what naming them does.
    // A MODULE MAY BE NAMED OR SPELLED OUT.
    //
    // `--module doom2` and `--module modules/doom2` both work: the first is
    // what a person types, the second is what a script that already knows the
    // layout passes. Resolved here, once, so the server and the web server
    // accept the same thing -- they did not, and `run_stack.sh --module doom2`
    // handed the web server a name it resolved and the server a path it could
    // not find, which presented as "no module has any maps".
    const auto has_dir = [](const std::string& p) {
        DIR* d = opendir(p.c_str());
        if (d == nullptr) return false;
        closedir(d);
        return true;
    };
    static std::vector<std::string> module_resolved;
    for (u32 mi = 0; mi < module_count; ++mi) {
        std::string given = module_dirs[mi];
        if (!has_dir(given + "/maps") && has_dir("modules/" + given + "/maps")) {
            given = "modules/" + given;
        }
        module_resolved.push_back(given);
    }
    for (u32 mi = 0; mi < module_count; ++mi) {
        module_dirs[mi] = module_resolved[mi].c_str();
    }

    static std::vector<std::string> discovered;
    if (map_count == 0 && module_count > 0) {
        for (u32 mi = 0; mi < module_count; ++mi) {
            std::string dir = std::string(module_dirs[mi]) + "/maps";
            DIR* d = opendir(dir.c_str());
            if (d == nullptr) continue;
            std::vector<std::string> found;
            while (dirent* e = readdir(d)) {
                const std::string n = e->d_name;
                if (n.size() > 5 && n.compare(n.size() - 5, 5, ".dnpm") == 0) {
                    found.push_back(dir + "/" + n);
                }
            }
            closedir(d);
            std::sort(found.begin(), found.end());
            for (const std::string& f : found) discovered.push_back(f);
        }
        for (const std::string& f : discovered) {
            if (map_count < MAX_LEVELS) map_paths[map_count++] = f.c_str();
        }
        if (map_count > 0) {
            std::fprintf(stderr, "[srv] %u map(s) from the module(s), in name order\n",
                         map_count);
        }
        if (discovered.size() > map_count) {
            std::fprintf(stderr,
                "[srv] the module has %zu maps and only %u fit; name the ones you "
                "want with --map\n", discovered.size(), map_count);
        }
    }

    if (map_count == 0) {
        std::fprintf(stderr, "[srv] no --map given and no module has any; "
                             "nothing to host\n");
        return 1;
    }
    for (u32 k = 0; k < map_count; ++k) srv.declare_level(map_paths[k]);

    // THE WORLD'S LAYOUT, if anything says. Named outright, or found beside the
    // maps of the first module that has one -- discovered the same way the maps
    // themselves are, so a module that lays its levels out ships the file and
    // one that does not is unchanged in every respect.
    //
    // A metamap that will not parse is FATAL rather than ignored: a world whose
    // layout was half read is a world where some neighbours load and some do
    // not, which is the kind of fault that presents as an occasional stall.
    {
        std::string mm = metamap_path ? metamap_path : std::string();
        if (mm.empty()) {
            for (u32 mi = 0; mi < module_count; ++mi) {
                const std::string candidate =
                    std::string(module_dirs[mi]) + "/maps/metamap.txt";
                std::FILE* probe = std::fopen(candidate.c_str(), "rb");
                if (probe) { std::fclose(probe); mm = candidate; break; }
            }
        }
        if (!mm.empty() && !srv.load_metamap(mm.c_str())) return 1;
    }
    std::fprintf(stderr, "[srv] %u level(s) declared; an exit advances to the next\n",
                 srv.level_count());

    // The prop catalogue.
    //
    // --props names one outright. Otherwise a module's own is tried first and
    // the engine's is the fallback -- per FILE, so a module that ships no
    // catalogue gets the engine's rather than none. Missing is not fatal: a map
    // that spawns nothing does not need one, and a map that DOES fails loudly
    // at the spawn rather than quietly producing a key nobody can see.
    if (!srv.props.load(props_path == nullptr ? "assets/props.txt" : props_path)) {
        std::fprintf(stderr, "[srv] no prop catalogue at %s; scripts cannot spawn props\n",
                     props_path == nullptr ? "assets/props.txt" : props_path);
    }

    // Then every module's, ADDED rather than replacing. Ids are absolute and
    // are not renumbered, so a collision is a real conflict between two
    // projects and the server refuses to start: serving a level where some of
    // the barrels are drawn as keys is worse than not serving it.
    u32 module_collisions = 0;
    for (u32 mi = 0; mi < module_count; ++mi) {
        char path[512];
        std::snprintf(path, sizeof(path), "%s/assets/props.txt", module_dirs[mi]);
        if (std::FILE* f = std::fopen(path, "rb")) {
            std::fclose(f);
            srv.props.add(path);
            module_collisions += srv.props.collisions();
        }
        std::fprintf(stderr, "[srv] module %s\n", module_dirs[mi]);
    }
    if (module_collisions > 0) {
        std::fprintf(stderr,
            "[srv] %u prop id collision(s) between modules -- see the lines above. "
            "An id is the WIRE id, so two modules cannot share one; give each "
            "module its own range (docs/MODULES.md).\n", module_collisions);
        return 1;
    }

    // A level gets a VM if the operator supplied scripts OR the map declared
    // any of its own. That is the point of the map declaring them: a converted
    // Doom level works when you load it, not when you remember which files to
    // pass. --no-scripts is the operator's veto over both.
    //
    // ASKED PER LEVEL, AT LOAD, and it has to be: the question is "does this
    // map declare scripts", the answer is in the map, and with levels loaded on
    // demand there is no map here to ask. This used to be a scan over every
    // level's map_store at startup, which after the split read a payload that
    // does not exist yet -- a null dereference before the first tick.
    if (!no_scripts) {
        srv.vm_config.trust = strict_scripts ? lua::Trust::Strict : lua::Trust::Full;
        if (lua_stall_us > 0) srv.vm_config.stall_warn_us = lua_stall_us;
        srv.hash_trace = hash_trace_every;
        srv.test_player = test_player;
        srv.scripts_enabled = true;
        // Stated at startup, deliberately. Under the default profile a script
        // can reach the filesystem, databases and native libraries, and the
        // confinement that makes that safe lives outside this process. An
        // operator should learn their trust posture from the log rather than
        // from an incident.
        std::fprintf(stderr, "[srv] Lua: trust=%s (filesystem/db/native %s), "
                             "budget %u/call %u/tick, memory cap %zu MB\n",
                     strict_scripts ? "strict" : "full",
                     strict_scripts ? "DENIED" : "ALLOWED -- confine this process",
                     srv.vm_config.budget_per_call, srv.vm_config.budget_per_tick,
                     srv.vm_config.memory_cap / (1024u * 1024u));

        // KEPT, not applied. Every level's scripts are loaded when its payload
        // is, which is when somebody comes near it -- so the list the operator
        // gave has to outlive this block rather than be spent in it.
        srv.shared_scripts.assign(script_args, script_args + script_count);
    }

    // LEVEL 0 EAGERLY, and only level 0.
    //
    // Two reasons, both about the moment a mistake is noticed. A map that does
    // not load is a fatal startup error and always has been -- discovering it
    // when the first player walks in would turn "the server refuses to start"
    // into "the game broke forty minutes later" -- and the entry map is the one
    // every session is placed in, so loading it here is the difference between
    // a join that is instant and a join that waits for a nav build.
    //
    // The other thirty-one wait. That is the whole change: they are declared,
    // they are listed, an exit still advances to them, and they cost a path
    // and a tick counter until somebody is near enough to want one.
    if (srv.level_count() > 0 && !srv.load_level(0)) {
        std::fprintf(stderr, "[srv] refusing to start without the entry map\n");
        return 1;
    }
    // AND THE ENTRY MAP'S NEIGHBOURS, before anybody arrives. The first player
    // would otherwise pay for them inside their own handshake, which is the one
    // moment a load is least welcome.
    srv.refresh_residency();

    // The stand-in body, spawned before the first tick so both runs of a
    // scenario start from the same state at the same tick.
    if (srv.test_player) {
        Level& L = srv.level(0);
        const fx3 at = L.map_bytes.empty()
            ? fx3{fx::from_int(10), fx::from_int(10), fx::from_int(2)}
            : L.map_store.spawn_for_tag(srv.spawn_tag, srv.spawn_tag_len);
        const u32 a = L.world.spawn(at);
        if (a < sim::MAX_AGENTS) {
            L.hp[a] = 100;
            physics::MoveParams& p = L.world.params_mut(a);
            p.radius = L.map_store.body_radius();
            p.height = L.map_store.body_height();
            L.effects.set_base(a, L.world.params(a));
            // kind 0 and NOT scripted, which together are what every mob
            // script means by "a player": nobody placed it, so it walked in.
            std::fprintf(stderr, "[srv] --test-player: agent %u at the '%s' tag; "
                                 "level 0 will tick from tick 0\n", a, srv.spawn_tag);
        } else {
            std::fprintf(stderr, "[srv] --test-player: no agent slot free\n");
        }
    }

    if (!srv.link.listen(port)) {
        std::fprintf(stderr, "[srv] failed to listen on %u\n", port);
        return 1;
    }
    std::fprintf(stderr, "[srv] listening for edge on 127.0.0.1:%u, %u Hz\n",
                 port, sim::TICK_RATE_HZ);

    srv.link.on_session_open([&](u32 id) { srv.on_open(id); });
    srv.link.on_session_close([&](u32 id, u16 r) { srv.on_close(id, r); });
    srv.link.on_control([&](u32 id, const u8* d, size_t n) { srv.on_control(id, d, n); });
    srv.link.on_datagram([&](u32 id, const u8* d, size_t n) { srv.on_datagram(id, d, n); });

    auto next_tick = Clock::now() + TICK_PERIOD;
    u32 tick_counter = 0;

    for (;;) {
        const auto now = Clock::now();
        auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next_tick - now).count();
        if (wait < 0) wait = 0;
        if (wait > 50) wait = 50;

        if (!srv.link.poll(static_cast<int>(wait))) break;

        if (Clock::now() >= next_tick) {
            // The WHOLE tick, measured from out here, so the report cannot
            // quietly omit a phase that was added later: if lua + step + snap
            // stops adding up to this, something is being done that nothing is
            // counting, and the report says so by not summing.
            const u64 t_tick0 = srv.perf.every ? Server::now_us() : 0;
            srv.tick();
            u64 snap_us = 0;
            if (++tick_counter % srv.snapshot_divisor == 0) {
                const u64 t_snap0 = srv.perf.every ? Server::now_us() : 0;
                srv.send_snapshots();
                if (srv.perf.every) snap_us = Server::now_us() - t_snap0;
            }
            if (srv.perf.every) {
                const u64 whole = Server::now_us() - t_tick0;
                if (whole > srv.perf.worst_total_us) srv.perf.worst_total_us = whole;
                srv.perf_report(snap_us);
            }

            next_tick += TICK_PERIOD;
            // If the loop fell far behind (a long stall, a debugger break), do
            // NOT try to catch up by running a burst of ticks: that would make
            // the server briefly simulate faster than the client predicts and
            // guarantee a wave of mispredictions. Drop the missed time instead.
            const auto behind = Clock::now() - next_tick;
            if (behind > std::chrono::milliseconds(250)) {
                std::fprintf(stderr, "[srv] fell %lld ms behind; dropping missed ticks\n",
                             static_cast<long long>(
                                 std::chrono::duration_cast<std::chrono::milliseconds>(behind).count()));
                next_tick = Clock::now() + TICK_PERIOD;
            }
        }
    }
    return 0;
}
