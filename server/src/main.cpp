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
#include "dnp/sim/testmap.hpp"
#include "dnp/sim/world.hpp"
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
    std::vector<u8>                   map_bytes;   // exactly what clients receive
    u64                               map_hash = 0;

    Server() : map_store(new geom::MapStorage()), nav_graph(new nav::NavGraph()) {
        // Until a map is loaded, fall back to the built-in one so the server is
        // still runnable. Phase 3 retires this once every deployment ships a
        // baked map.
        world.set_map(&sim::test_map());
    }

    // Scripted-discrete movers: how far each may travel, and the schedule state
    // driving it. Indexed by sector so a map may have several.
    fx  discrete_range[geom::MAX_MAP_SECTORS]{};
    fx  discrete_now[geom::MAX_MAP_SECTORS]{};
    fx  discrete_target[geom::MAX_MAP_SECTORS]{};
    Rng discrete_rng{0xD15C0FFEEull, 7};

    // Advance every scripted-discrete mover by one tick.
    //
    // Deliberately NOT a function of the tick counter. A schedule a client
    // could re-derive would make this a parametric mover wearing a disguise,
    // and the whole reason the protocol streams SectorOffset every snapshot is
    // that no such derivation exists.
    void drive_discrete_movers() {
        for (u32 si = 0; si < geom::MAX_MAP_SECTORS; ++si) {
            if (discrete_range[si].raw == 0) continue;

            // A new destination every so often, then a steady crawl toward it.
            // Crawling rather than jumping because an entity riding it is
            // carried rigidly: a mover that teleported would teleport its
            // passengers, which is a gameplay choice a map author should have
            // to make on purpose rather than inherit from the scheduler.
            if ((discrete_rng.next_u32() & 63u) == 0) {
                discrete_target[si] =
                    fx::from_frac(static_cast<i64>(discrete_rng.next_range(0, 101)), 100) *
                    discrete_range[si];
            }
            const fx step = fx::from_frac(1, 100);
            if (discrete_now[si] < discrete_target[si]) {
                discrete_now[si] += step;
                if (discrete_now[si] > discrete_target[si]) discrete_now[si] = discrete_target[si];
            } else if (discrete_now[si] > discrete_target[si]) {
                discrete_now[si] -= step;
                if (discrete_now[si] < discrete_target[si]) discrete_now[si] = discrete_target[si];
            }
            world.motions().set_discrete(si, fx3{fx::zero(), fx::zero(), discrete_now[si]});
        }
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

    void send_motion_sets(u32 id) {
        const auto& ms = world.motions();
        for (u32 i = 0; i < ms.count(); ++i) {
            const sim::SectorMotion& m = ms.at(i);
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

        // Sectors tagged 100+ are movers, so the authored map decides what
        // moves rather than the server hardcoding indices. Lua takes this over
        // in Phase 4; the tag convention is the bridge.
        //
        //   100+N  a lift that rises N units, travelling as a volume
        //   200+N  a crusher whose CEILING descends N units
        //   300+N  a footprint that slides N units south (-y)
        //
        // Three conventions rather than one because the differences are not
        // decorative: only a sector that moves one surface can ever close the
        // gap a body occupies (§3.1), and only one that moves horizontally can
        // sweep a body out of its way (§3.1.3).
        for (u32 si = 0; si < loaded.sector_count; ++si) {
            const u16 tag = loaded.sector(si).tag;
            if (tag < 100) continue;
            if (world.motions().add(*map_store, si) >= sim::MAX_MOVING_SECTORS) continue;

            if (tag >= 400) {
                // A scripted-discrete mover (design §3.1.2). There is no path
                // to declare, because the whole point is that there is no
                // closed form: the offset is whatever a callback decides this
                // tick. Lua is Phase 4, so the stand-in is the server's own
                // PRNG -- and that is faithful, because what makes this kind
                // unpredictable to a client is not randomness but the absence
                // of any parameter set describing it.
                discrete_range[si] = fx::from_int(tag - 400);
                world.motions().set_discrete(si, fx3::zero());
                std::fprintf(stderr, "[srv] sector %u drifts (scripted-discrete, range %u)\n",
                             si, tag - 400u);
            } else if (tag >= 300) {
                const u16 slide = tag - 300;
                world.motions().start_parametric(
                    si, fx3::zero(), fx3{fx::zero(), -fx::from_int(slide), fx::zero()},
                    0, 240, sim::LoopMode::PingPong);
                std::fprintf(stderr, "[srv] sector %u sweeps (slides %u south)\n", si, slide);
            } else if (tag >= 200) {
                const u16 drop = tag - 200;
                world.motions().start_parametric(
                    si, fx3::zero(), fx3{fx::zero(), fx::zero(), -fx::from_int(drop)},
                    0, 240, sim::LoopMode::PingPong, sim::MotionTarget::Ceiling);
                std::fprintf(stderr, "[srv] sector %u crushes (ceiling drops %u)\n", si, drop);
            } else {
                // Ping-pong a lift over four seconds. A placeholder schedule
                // until Lua drives it.
                world.motions().start_parametric(
                    si, fx3::zero(), fx3{fx::zero(), fx::zero(), fx::from_int(tag - 100)},
                    0, 240, sim::LoopMode::PingPong);
                std::fprintf(stderr, "[srv] sector %u moves (rise %u)\n", si, tag - 100u);
            }
        }
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
        if (it->second.has_agent) world.despawn(it->second.agent);
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

    void tick() {
        for (auto& [id, s] : sessions) {
            if (s.has_agent) world.set_input(s.agent, s.take());
        }
        // Before the step, so the offsets a snapshot reports are the ones the
        // tick actually ran with. Deciding after would ship the client an
        // offset the server had not used yet.
        drive_discrete_movers();
        world.step();
        report_crushes();
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
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = static_cast<u16>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            map_path = argv[++i];
        } else if (std::strcmp(argv[i], "--spawn") == 0 && i + 1 < argc) {
            spawn_tag_arg = argv[++i];
        }
    }

    Server srv;
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
