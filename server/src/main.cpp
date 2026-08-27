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
#include <cstring>
#include <map>
#include <thread>

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
#include "lua/world_binding.hpp"

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
constexpr size_t MAP_CHUNK_BYTES = 8192;

struct Server {
    net::LinkServer      link;
    sim::World           world;

    // Heap, not a member by value: MapStorage is a few hundred KB of fixed
    // arrays and Server is a stack local.
    // Which named spawn arriving players land on. Portals supply this per
    // player via entry_spawn_tag (design §3.3); until then it is server-wide.
    const char* spawn_tag = "start";
    u32         spawn_tag_len = 5;

    std::unique_ptr<geom::MapStorage> map_store;

    // The sector graph (design §3.2.4). Nothing routes over it yet -- enemies
    // are Phase 4 -- but building it at load turns a map whose rooms cannot
    // reach each other into a line in the startup log rather than into an
    // enemy that stands still and a bug report six weeks later.
    std::unique_ptr<nav::NavGraph>    nav_graph;

    // The script runtime. Nothing is bound to it yet -- that is the next slice
    // -- but it runs on the tick so its budget, GC and stall accounting are
    // exercised from the first commit rather than bolted on once scripts
    // already depend on the timing.
    lua::Vm            script_vm;
    const char*        script_path = nullptr;
    lua::CommandQueue  lua_commands;
    lua::EffectStack   lua_effects;
    lua::WorldContext  lua_ctx;

    // The tick at which a script last changed each agent. Compared against the
    // tick of the last snapshot sent to a session, so LUA_APPLIED is set for
    // the observers a change actually concerns and nobody else -- setting it
    // globally would excuse every client's mismatch on every scripted tick.
    u32 lua_touched_tick[sim::MAX_AGENTS]{};
    std::vector<u8>                   map_bytes;   // exactly what clients receive
    u64                               map_hash = 0;

    Server() : map_store(new geom::MapStorage()), nav_graph(new nav::NavGraph()) {
        // Until a map is loaded, fall back to the built-in one so the server is
        // still runnable. Phase 3 retires this once every deployment ships a
        // baked map.
        world.set_map(&sim::test_map());
    }

    // Sectors whose motion is scripted-discrete, collected once per tick so the
    // snapshot encoder does not rescan.
    proto::SectorOffset discrete_offsets[proto::MAX_SECTOR_OFFSETS];
    u16                 discrete_count = 0;

    void collect_discrete_offsets() {
        discrete_count = 0;
        const auto& ms = world.motions();
        for (u32 i = 0; i < ms.count() && discrete_count < proto::MAX_SECTOR_OFFSETS; ++i) {
            const sim::SectorMotion& m = ms.at(i);
            // Parametric movers are deliberately absent: the client computes
            // theirs from the parameters it already holds, and sending it too
            // would create a second source of truth that could disagree.
            if (m.type != sim::MotionType::ScriptedDiscrete) continue;
            discrete_offsets[discrete_count].sector = m.sector;
            discrete_offsets[discrete_count].offset = m.offset;
            ++discrete_count;
        }
    }

    void send_motion(u32 id, const sim::SectorMotion& m) {
        u8 buf[128];
        ByteWriter w(buf, sizeof(buf));
        const size_t len_pos = w.mark();
        w.u32v(0);
        w.u8v(proto::MSG_SECTOR_MOTION_SET);
        w.u32v(m.sector);
        w.u8v(static_cast<u8>(m.type));
        w.u8v(static_cast<u8>(m.target));
        w.u32v(m.start_tick);
        w.u32v(m.duration_ticks);
        w.u8v(static_cast<u8>(m.loop));
        w.vec3_raw(m.from);
        w.vec3_raw(m.to);
        const size_t end = w.size();
        const u32 body = static_cast<u32>(end - len_pos - 4);
        buf[len_pos]     = static_cast<u8>(body & 0xFF);
        buf[len_pos + 1] = static_cast<u8>((body >> 8) & 0xFF);
        buf[len_pos + 2] = static_cast<u8>((body >> 16) & 0xFF);
        buf[len_pos + 3] = static_cast<u8>((body >> 24) & 0xFF);
        link.send_control(id, buf, end);
    }

    void send_motion_sets(u32 id) {
        const auto& ms = world.motions();
        for (u32 i = 0; i < ms.count(); ++i) send_motion(id, ms.at(i));
    }

    // Two parametric motions are the same motion iff a client computing either
    // one would land on the same offset for every tick. That is exactly the
    // descriptor -- endpoints, start, duration, loop, target -- and NOT the
    // current offset, which is derived from it.
    static bool same_motion(const sim::SectorMotion& a, const sim::SectorMotion& b) {
        return a.sector == b.sector && a.type == b.type && a.target == b.target &&
               a.start_tick == b.start_tick && a.duration_ticks == b.duration_ticks &&
               a.loop == b.loop && a.from == b.from && a.to == b.to;
    }

    // A door opened mid-match has to reach the clients, or they predict against
    // a sector that never moved.
    //
    // Sent on CHANGE rather than every tick, and on the reliable channel: a
    // parametric motion is a closed form, so one message buys every tick of it
    // forever (§3.1.2) -- but only if the message arrives. Losing it would leave
    // a client walking into a door that is open on the server, which no later
    // snapshot repairs, because snapshots carry entities and not geometry.
    void replicate_motion_changes() {
        const auto& ms = world.motions();
        for (u32 i = 0; i < ms.count(); ++i) {
            const sim::SectorMotion& m = ms.at(i);
            if (i < sent_motion_count && same_motion(sent_motions[i], m)) continue;
            sent_motions[i] = m;
            motion_changed_tick = world.tick() + 1;
            for (auto& [id, s] : sessions) {
                if (s.hello_done) send_motion(id, m);
            }
        }
        if (ms.count() > sent_motion_count) sent_motion_count = ms.count();
    }

    // --- movement parameters (docs/protocol.md 8.4) --------------------------
    //
    // The parameter table is the RULES the client predicts with, not state it
    // can be corrected toward. A client running different parameters mispredicts
    // every tick and never recovers, so these go on the reliable channel and
    // carry a generation the snapshot echoes back.

    u8 param_epoch[sim::MAX_AGENTS]{};

    // Send one agent's parameters. `full` sends every field, which is what a
    // joining client needs because it has no history to patch.
    void send_param_update(u32 id, u32 agent, bool full,
                           const u16* ids = nullptr, u16 id_count = 0) {
        proto::ParamUpdate p;
        p.handle         = proto::Handle::make(agent + 1, world.generation(agent));
        p.epoch          = param_epoch[agent];
        p.effective_tick = world.tick();
        p.flags          = full ? proto::param_flag::FULL : 0;

        const physics::MoveParams& mp = world.params(agent);
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
    void set_param(u32 agent, u16 field_id, i64 raw) {
        if (agent >= sim::MAX_AGENTS) return;
        if (!physics::param_set_raw(world.params_mut(agent), field_id, raw)) return;
        // Wraps deliberately; only equality is ever tested (docs/protocol.md
        // 10.1.2), and 255 intervening changes cannot happen inside one round
        // trip.
        param_epoch[agent] = static_cast<u8>(param_epoch[agent] + 1);
        for (auto& [id, s] : sessions) {
            if (s.hello_done) send_param_update(id, agent, false, &field_id, 1);
        }
    }

    // A stand-in for Lua, kept only so PARAM_UPDATE replication can be tested
    // without a script attached. Sector motion no longer needs an equivalent:
    // maps/scripts/init.lua drives it.
    bool param_test = false;
    void drive_param_test() {
        if (!param_test) return;
        if (world.tick() % 120 != 0) return;
        const i32 id = physics::param_id_by_name("move_speed");
        if (id < 0) return;
        const bool fast = (world.tick() / 120) % 2 == 1;
        const fx want = fast ? fx::from_int(16) : fx::from_int(8);
        for (auto& [sid, s] : sessions) {
            if (!s.has_agent) continue;
            if (world.params(s.agent).move_speed == want) continue;
            set_param(s.agent, static_cast<u16>(id), want.raw);
            std::fprintf(stderr, "[srv] tick %u: agent %u move_speed -> %lld (epoch %u)\n",
                         world.tick(), s.agent,
                         static_cast<long long>(want.raw >> 32), param_epoch[s.agent]);
        }
    }

    bool load_map_file(const char* path) {
        std::FILE* f = std::fopen(path, "rb");
        if (!f) { std::fprintf(stderr, "[srv] cannot open map %s\n", path); return false; }
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n <= 0) { std::fclose(f); return false; }
        map_bytes.resize(static_cast<size_t>(n));
        const size_t got = std::fread(map_bytes.data(), 1, map_bytes.size(), f);
        std::fclose(f);
        if (got != map_bytes.size()) { map_bytes.clear(); return false; }

        ByteReader r(map_bytes.data(), map_bytes.size());
        const geom::MapLoadStatus st = geom::load_map(r, *map_store);
        if (st != geom::MapLoadStatus::Ok) {
            std::fprintf(stderr, "[srv] map %s rejected: %s\n", path,
                         geom::map_load_status_text(st));
            map_bytes.clear();
            return false;
        }

        // Point the world at the loaded map. The view is stable for the
        // lifetime of the store, which outlives the world.
        static geom::MapData loaded;
        loaded = map_store->view();
        world.set_map(&loaded);
        map_hash = map_store->hash();
        world.set_map_storage(map_store.get());

        // Sector motion is the MAP'S business now, not the engine's.
        //
        // This is where the 100/200/300/400 tag branches used to live, marked
        // "Lua takes this over in Phase 4". maps/scripts/init.lua implements the
        // same convention, so every existing map behaves identically -- what
        // changed is who decides. Run without --script and nothing moves, which
        // is the control that proves the takeover is real rather than a second
        // implementation running alongside the first.

        std::fprintf(stderr, "[srv] map %s: %u sectors, %u sides, hash %016llX\n",
                     path, loaded.sector_count, loaded.side_count,
                     static_cast<unsigned long long>(map_hash));

        // Nav graph, plus a reachability report from the default spawn. This is
        // a map-authoring signal, not a runtime one: "17 of 40 sectors are
        // reachable" is something the author wants to hear at load, and is
        // otherwise only discovered by watching an enemy fail to path.
        if (!nav_graph->build(loaded)) {
            std::fprintf(stderr, "[srv] map has more sectors than the nav graph holds\n");
            return false;
        }
        report_reachability(loaded);
        return true;
    }

    // How much of the map a standard body can walk to from the spawn.
    //
    // A flood fill over the same edge test A* uses, so it reports what pathing
    // would actually find rather than what the topology suggests. Sectors are
    // counted once; the queue is the caller's stack-free scratch.
    void report_reachability(const geom::MapData& map) {
        static u8  seen[nav::MAX_NAV_NODES];
        static u32 queue[nav::MAX_NAV_NODES];
        for (u32 i = 0; i < nav_graph->node_count(); ++i) seen[i] = 0;

        const fx3 origin = map_store->spawn_for_tag(spawn_tag, spawn_tag_len);
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
            const nav::NavNode& n = nav_graph->node(cur);
            for (u32 k = 0; k < n.edge_count; ++k) {
                const nav::NavEdge& e = nav_graph->edge(n.first_edge + k);
                if (seen[e.to]) continue;
                if (!nav_graph->edge_is_open(map, cur, e, body)) continue;
                seen[e.to] = 1;
                queue[tail++] = e.to;
                ++found;
            }
        }

        std::fprintf(stderr, "[srv] nav: %u nodes, %u edges; %u of %u sectors reachable "
                             "on foot from spawn '%s'\n",
                     nav_graph->node_count(), nav_graph->edge_count(), found,
                     nav_graph->node_count(), spawn_tag);
        if (found < nav_graph->node_count()) {
            // Not an error. Sectors reachable only by jumping, grappling or a
            // lift at the top of its travel are legitimate level design, and
            // the walk-only graph is expected to miss them (design §3.2.2:
            // enemies do not jump). Worth saying out loud all the same.
            std::fprintf(stderr, "[srv] nav: %u sector(s) need more than walking to reach\n",
                         nav_graph->node_count() - found);
        }
    }

    void send_map(u32 id) {
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
            const u32 body = static_cast<u32>(end - len_pos - 4);
            msg[len_pos]     = static_cast<u8>(body & 0xFF);
            msg[len_pos + 1] = static_cast<u8>((body >> 8) & 0xFF);
            msg[len_pos + 2] = static_cast<u8>((body >> 16) & 0xFF);
            msg[len_pos + 3] = static_cast<u8>((body >> 24) & 0xFF);
            link.send_control(id, msg.data(), end);
        }
    }
    std::map<u32, Session> sessions;   // ordered: iteration order must not vary
    u32                  snapshot_divisor = 2;   // 60 Hz sim, 30 Hz snapshots

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
            lua_effects.clear_agent(it->second.agent);
            world.despawn(it->second.agent);
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

            // Spawn at the map's tagged landing point. entry_spawn_tag comes
            // from the portal that sent the player (design §3.3); until portals
            // land, everyone arrives at the default.
            const fx3 spawn_pos = map_bytes.empty()
                ? fx3{fx::from_int(10), fx::from_int(10), fx::from_int(2)}
                : map_store->spawn_for_tag(spawn_tag, spawn_tag_len);
            const u32 agent = world.spawn(spawn_pos);
            if (agent >= sim::MAX_AGENTS) {
                u8 buf[64];
                ByteWriter w(buf, sizeof(buf));
                w.u32v(1 + 2);
                w.u8v(proto::MSG_REJECT);
                w.u16v(2);   // reason 2: server full
                link.send_control(id, buf, w.size());
                link.close_session(id, 2);
                return;
            }

            // Capture the authored parameters as this agent's base. Everything
            // effects do is computed from here, never from the live value --
            // see server/src/lua/effects.hpp for why that matters.
            lua_effects.set_base(agent, world.params(agent));

            s.agent = agent;
            s.has_agent = true;
            s.hello_done = true;

            u8 buf[128];
            ByteWriter w(buf, sizeof(buf));
            const size_t len_pos = w.mark();
            w.u32v(0);   // patched below
            w.u8v(proto::MSG_WELCOME);
            w.u16v(proto::PROTOCOL_VERSION);
            w.u64v(0x444E5030u);                       // server_id
            w.u16v(proto::TICK_RATE_HZ);
            w.u16v(static_cast<u16>(sim::TICK_RATE_HZ / snapshot_divisor));
            w.u32v(world.tick());
            w.u32v(proto::Handle::make(agent + 1, world.generation(agent)).raw);
            w.u64v(map_hash);
            const size_t end = w.size();
            const u32 body_len = static_cast<u32>(end - len_pos - 4);
            buf[len_pos]     = static_cast<u8>(body_len & 0xFF);
            buf[len_pos + 1] = static_cast<u8>((body_len >> 8) & 0xFF);
            buf[len_pos + 2] = static_cast<u8>((body_len >> 16) & 0xFF);
            buf[len_pos + 3] = static_cast<u8>((body_len >> 24) & 0xFF);
            link.send_control(id, buf, end);

            // The map follows WELCOME immediately. A client must not start
            // predicting before it lands (docs/protocol.md 8.0) -- physics
            // against no geometry is wrong on every tick, and the corrections
            // would arrive as a burst of mispredictions explained nowhere near
            // the symptom.
            send_map(id);
            // Motion parameters follow the map. Parametric movers cost nothing
            // after this; the client computes them for any tick.
            send_motion_sets(id);
            // And the movement parameters. FULL, because a joining client has
            // no history to patch (docs/protocol.md 8.4). Sent before the first
            // snapshot so the client is never predicting with rules it has not
            // been told.
            send_param_update(id, agent, true);

            std::fprintf(stderr, "[srv] session %u -> agent %u\n", id, agent);
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
        it->second.ingest(b);
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
    u32 crush_reported_tick[sim::MAX_AGENTS]{};
    bool crush_seen[sim::MAX_AGENTS]{};

    // The motion descriptors every client has been told about, so a change can
    // be recognised as a change. Indexed the same way SectorMotionSet indexes
    // its own -- motions are never removed, only redefined, so the index is
    // stable for the life of the map.
    sim::SectorMotion sent_motions[sim::MAX_MOVING_SECTORS]{};
    u32 sent_motion_count = 0;
    // The tick a motion descriptor last changed, for the same +1 reason
    // lua_touched_tick uses: "since the last snapshot" is a strict comparison.
    u32 motion_changed_tick = 0;

    void report_crushes() {
        bool now[sim::MAX_AGENTS]{};
        const sim::CrushLog& log = world.crushes();
        for (u32 i = 0; i < log.count(); ++i) {
            const sim::CrushEvent& e = log.at(i);
            if (e.entity >= sim::MAX_AGENTS) continue;
            now[e.entity] = true;
            if (crush_seen[e.entity]) continue;   // already reported this episode
            crush_seen[e.entity] = true;
            crush_reported_tick[e.entity] = world.tick();
            std::fprintf(stderr, "[srv] tick %u: agent %u %s by sector %u (depth %.3f)\n",
                         world.tick(), e.entity,
                         e.kind == sim::CrushKind::Vertical ? "crushed" : "pinned",
                         e.sector, static_cast<double>(e.depth.raw) / 4294967296.0);
        }
        // An episode ends when the geometry lets go, so escaping and being
        // caught again reports twice.
        for (u32 i = 0; i < sim::MAX_AGENTS; ++i) {
            if (!now[i] && crush_seen[i]) {
                std::fprintf(stderr, "[srv] tick %u: agent %u released after %u ticks\n",
                             world.tick(), i, world.tick() - crush_reported_tick[i]);
                crush_seen[i] = false;
            }
        }
        if (log.dropped() > 0) {
            // Never silently cap -- the same rule R10 applies to snapshots.
            std::fprintf(stderr, "[srv] tick %u: %u crush events dropped (log full)\n",
                         world.tick(), log.dropped());
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
    void dispatch_events() {
        if (!script_vm.is_open()) return;
        const u32 t = world.tick();

        const sim::CrushLog& crushes = world.crushes();
        for (u32 i = 0; i < crushes.count(); ++i) {
            const sim::CrushEvent& e = crushes.at(i);
            if (e.entity >= sim::MAX_AGENTS) continue;
            // Only the first tick of an episode, matching what report_crushes
            // logs: a script that took damage sixty times a second while a
            // ceiling sat on someone would be a different design decision, and
            // one a script can make for itself from on_tick.
            if (crush_reported_tick[e.entity] != t || !crush_seen[e.entity]) continue;
            if (!script_vm.begin_hook("on_crush")) break;
            lua::push_entity(script_vm.state(), lua_ctx, e.entity);
            lua::push_sector(script_vm.state(), e.sector);
            lua_pushstring(script_vm.state(),
                           e.kind == sim::CrushKind::Vertical ? "vertical" : "pinned");
            script_vm.end_hook("on_crush", t, 3);
        }

        const sim::InteractLog& uses = world.interactions();
        for (u32 i = 0; i < uses.count(); ++i) {
            const sim::InteractEvent& e = uses.at(i);
            if (e.entity >= sim::MAX_AGENTS || e.sector < 0) continue;
            const char* hook = e.kind == sim::TouchKind::Use ? "on_interact" : "on_cross";
            if (!script_vm.begin_hook(hook)) continue;
            lua::push_entity(script_vm.state(), lua_ctx, e.entity);
            // The SIDE INDEX, as a plain number, because that is the key a
            // generated script indexes its table of doors by. dnp.side(i)
            // turns it into neighbour and flags when the script wants them.
            lua_pushnumber(script_vm.state(), e.side);
            lua::push_sector(script_vm.state(), static_cast<u32>(e.sector));
            script_vm.end_hook(hook, t, 3);
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
    void refresh_effect_params() {
        if (!script_vm.is_open()) return;

        const bool expired = lua_effects.expire(world.tick());
        if (!expired && !lua_ctx.effects_dirty) return;
        lua_ctx.effects_dirty = false;

        for (auto& [id, s] : sessions) {
            if (!s.has_agent) continue;
            const u32 a = s.agent;

            const physics::MoveParams want = lua_effects.compute(a);
            physics::MoveParams& live = world.params_mut(a);

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
            param_epoch[a] = static_cast<u8>(param_epoch[a] + 1);
            for (auto& [oid, os] : sessions) {
                if (os.hello_done) send_param_update(oid, a, false, changed, n);
            }
        }
    }

    void flush_lua_commands() {
        if (!script_vm.is_open()) return;
        const u32 now = world.tick();
        lua_commands.apply(world, [&](u32 agent) {
            if (agent < sim::MAX_AGENTS) lua_touched_tick[agent] = now + 1;
        });
        if (lua_commands.dropped() > 0) {
            // R10's rule again: never silently cap.
            std::fprintf(stderr, "[srv] tick %u: %u Lua commands dropped (queue full)\n",
                         now, lua_commands.dropped());
            lua_commands.clear();
        }
    }

    void tick() {
        for (auto& [id, s] : sessions) {
            if (s.has_agent) world.set_input(s.agent, s.take());
        }
        // Before the step, so the offsets a snapshot reports are the ones the
        // tick actually ran with. Deciding after would ship the client an
        // offset the server had not used yet.
        // Lua runs OUTSIDE world.step(), always. The client predicts by
        // running the same step; anything Lua did has to reach it as replicated
        // state, never as a second implementation of physics.
        if (script_vm.is_open()) {
            script_vm.begin_tick();
            // Timers and per-tick logic run BEFORE the step, in the same place
            // sector motion used to be driven from, and for the same reason:
            // what a snapshot reports must be what the tick actually ran with.
            script_vm.call_hook("on_tick", world.tick());
        }
        // After the callbacks, before the step: what the tick runs with is what
        // the snapshot will report.
        refresh_effect_params();

        drive_param_test();

        // Flush 1. Everything scripts asked for before the step lands here, so
        // physics never observes an entity moving underneath it mid-resolution.
        flush_lua_commands();

        world.step();
        report_crushes();
        dispatch_events();

        if (script_vm.is_open()) {
            // Post-step callbacks see the world the tick produced.
            script_vm.call_hook("on_post_tick", world.tick());
        }
        // Flush 2, before snapshots: a consequence and the state it acted on
        // ride the same snapshot rather than arriving a frame apart.
        flush_lua_commands();

        // And anything a script started this tick reaches the clients before
        // the snapshot that was produced with it does.
        replicate_motion_changes();

        if (script_vm.is_open()) {
            script_vm.end_tick(world.tick());
            // A fixed step per tick, so a collection cannot choose its own
            // moment, land on an arbitrary frame, push the loop past the 250 ms
            // threshold and present as a burst of client corrections.
            script_vm.gc_step();
        }
    }

    void send_snapshots() {
        // Once per snapshot, not once per session: the offsets are a property
        // of the world, and every observer of a moving sector sees the same one.
        collect_discrete_offsets();

        for (auto& [id, s] : sessions) {
            if (!s.has_agent) continue;

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
            if (lua_touched_tick[s.agent] > s.last_snapshot_sent ||
                motion_changed_tick > s.last_snapshot_sent) {
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
            sb.sector          = self.mv.sector;
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
                                     ? static_cast<u32>(self.mv.anchor) + 1u : 0u};
            sb.param_epoch     = param_epoch[s.agent];
            sb.state_hash      = world.agent_hash(s.agent);

            u8 buf[proto::MAX_DATAGRAM_BYTES];
            proto::SnapshotWriter w(buf, sizeof(buf));
            w.begin(h, sb, 100, discrete_offsets, discrete_count);

            SnapshotRecord& record = s.history.begin(world.tick());

            u32 omitted = 0;
            u32 unchanged = 0;
            for (u32 i = 0; i < sim::MAX_AGENTS; ++i) {
                // The observer's own agent is in SelfBlock at full precision.
                // Sending it again here would let the client reconcile against
                // the quantised copy of itself.
                if (i == s.agent) continue;

                const sim::AgentState& a = world.agent(i);
                const EntitySample was = baseline ? baseline->entities[i] : EntitySample{};

                EntitySample now;
                now.present          = a.active;
                now.generation       = world.generation(i);
                now.pos              = a.pos;
                now.facing           = a.facing;
                now.animation_state  = 0;

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

                if (w.try_entity(e)) {
                    record.entities[i] = now;
                } else {
                    // Did not fit. Record what the client STILL has, not what we
                    // wanted to send -- otherwise the next delta is computed
                    // against data that never arrived and the entity freezes at
                    // a stale position with nothing reporting an error.
                    record.entities[i] = was;
                    ++omitted;
                }
            }

            s.last_snapshot_sent = world.tick();
            const size_t n = w.finish();
            if (omitted > 0) {
                // Protocol R10: never silently cap.
                std::fprintf(stderr, "[srv] tick %u session %u: %u entities omitted (budget)\n",
                             world.tick(), id, omitted);
            }
            s.bytes_sent += n;
            s.snapshots_sent += 1;
            s.entities_omitted += unchanged;
            if (n > 0) link.send_datagram(id, buf, n);
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    u16 port = 34500;
    const char* map_path = nullptr;
    const char* spawn_tag_arg = nullptr;
    bool want_param_test = false;
    constexpr u32 MAX_SCRIPTS = 8;
    const char* script_args[MAX_SCRIPTS]{};
    u32 script_count = 0;
    bool strict_scripts = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = static_cast<u16>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            map_path = argv[++i];
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
        } else if (std::strcmp(argv[i], "--script-trust") == 0 && i + 1 < argc) {
            strict_scripts = std::strcmp(argv[++i], "strict") == 0;
        }
    }

    Server srv;
    srv.param_test = want_param_test;
    if (spawn_tag_arg) {
        srv.spawn_tag = spawn_tag_arg;
        srv.spawn_tag_len = static_cast<u32>(std::strlen(spawn_tag_arg));
    }
    if (map_path && !srv.load_map_file(map_path)) {
        std::fprintf(stderr, "[srv] refusing to start without the requested map\n");
        return 1;
    }
    if (!map_path) {
        std::fprintf(stderr, "[srv] no --map given; using the built-in test map "
                             "(clients cannot be sent geometry)\n");
    }
    if (script_count > 0) {
        lua::VmConfig vc;
        vc.trust = strict_scripts ? lua::Trust::Strict : lua::Trust::Full;
        if (!srv.script_vm.open(vc)) {
            std::fprintf(stderr, "[srv] cannot start Lua: %s\n", srv.script_vm.last_error());
            return 1;
        }
        // Stated at startup, deliberately. Under the default profile a script
        // can reach the filesystem, databases and native libraries, and the
        // confinement that makes that safe lives outside this process. An
        // operator should learn their trust posture from the log rather than
        // from an incident.
        std::fprintf(stderr, "[srv] Lua: trust=%s (filesystem/db/native %s), "
                             "budget %u/call %u/tick, memory cap %zu MB\n",
                     strict_scripts ? "strict" : "full",
                     strict_scripts ? "DENIED" : "ALLOWED -- confine this process",
                     vc.budget_per_call, vc.budget_per_tick,
                     vc.memory_cap / (1024u * 1024u));
        srv.lua_ctx.world = &srv.world;
        srv.lua_ctx.commands = &srv.lua_commands;
        srv.lua_ctx.effects = &srv.lua_effects;
        srv.lua_ctx.map_store = srv.map_store.get();
        // Seeded from the map, so a scripted-discrete schedule is unpredictable
        // to a client but replayable by the server.
        srv.lua_ctx.rng = Rng(srv.map_hash ^ 0x9E3779B97F4A7C15ull, 11);
        // Installed BEFORE the chunk runs: a script's top level is allowed to
        // look at the world, and `dnp` being nil at load time would be a
        // baffling first experience.
        lua::open_world(srv.script_vm.state(), &srv.lua_ctx);

        for (u32 k = 0; k < script_count; ++k) {
            if (!srv.script_vm.run_file(script_args[k], script_args[k])) {
                std::fprintf(stderr, "[srv] script failed: %s\n", srv.script_vm.last_error());
                return 1;
            }
            std::fprintf(stderr, "[srv] loaded script %s\n", script_args[k]);
        }
        srv.script_path = script_args[0];
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
            srv.tick();
            if (++tick_counter % srv.snapshot_divisor == 0) srv.send_snapshots();

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
