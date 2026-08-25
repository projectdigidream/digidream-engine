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
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>

#include "dnp/proto/messages.hpp"
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

struct Server {
    net::LinkServer      link;
    sim::World           world;
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

            const u32 agent = world.spawn(fx3{fx::zero(), fx::zero(), fx::from_int(2)});
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
            const size_t end = w.size();
            const u32 body_len = static_cast<u32>(end - len_pos - 4);
            buf[len_pos]     = static_cast<u8>(body_len & 0xFF);
            buf[len_pos + 1] = static_cast<u8>((body_len >> 8) & 0xFF);
            buf[len_pos + 2] = static_cast<u8>((body_len >> 16) & 0xFF);
            buf[len_pos + 3] = static_cast<u8>((body_len >> 24) & 0xFF);
            link.send_control(id, buf, end);

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

    void tick() {
        for (auto& [id, s] : sessions) {
            if (s.has_agent) world.set_input(s.agent, s.take());
        }
        world.step();
    }

    void send_snapshots() {
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

            proto::SelfBlock sb;
            sb.pos         = self.pos;
            sb.vel         = self.vel;
            sb.facing      = self.facing;
            sb.state_flags = self.state_flags;
            sb.jumps_used  = self.jumps_used;
            sb.state_hash  = world.agent_hash(s.agent);

            u8 buf[proto::MAX_DATAGRAM_BYTES];
            proto::SnapshotWriter w(buf, sizeof(buf));
            w.begin(h, sb, 100);

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
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = static_cast<u16>(std::atoi(argv[++i]));
        }
    }

    Server srv;
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
