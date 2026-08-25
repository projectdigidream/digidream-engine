#pragma once
//
// Per-session record of what was actually SENT, so snapshots can be delta-
// encoded against a baseline the client is known to hold (docs/protocol.md 5.3).
//
// The critical word is "actually". This records what left the server and was
// small enough to fit the datagram budget -- not what the server intended to
// send. Recording intent instead is the classic way to build a delta encoder
// that works until the budget is hit, at which point every subsequent delta is
// computed against state the client never received, and entities silently
// freeze at stale positions with no error anywhere.
//
#include "dnp/proto/messages.hpp"
#include "dnp/sim/world.hpp"

namespace dnp::server {

using namespace dnp;

// ~1 second of snapshots at 30 Hz. Needs to comfortably exceed the ack round
// trip: a client on a 300 ms link has ~9 snapshots in flight, and any ack older
// than the ring simply forces a full snapshot, which is correct but wasteful.
inline constexpr u32 SNAPSHOT_RING = 32;

struct EntitySample {
    bool present         = false;
    u8   generation      = 0;
    fx3  pos{};
    brad facing{};
    u16  animation_state = 0;
};

struct SnapshotRecord {
    u32  tick  = 0;
    bool valid = false;
    EntitySample entities[sim::MAX_AGENTS]{};
};

class SnapshotHistory {
public:
    void reset() noexcept {
        for (u32 i = 0; i < SNAPSHOT_RING; ++i) ring_[i] = SnapshotRecord{};
        next_ = 0;
    }

    /** Claim the next slot for `tick`, cleared and ready to be filled in. */
    SnapshotRecord& begin(u32 tick) noexcept {
        // Indexed by a rotating counter, NOT by `tick % SNAPSHOT_RING`.
        //
        // Snapshots go out every Nth tick, so snapshot ticks all share a
        // factor with the tick rate: at the default divisor of 2 they are all
        // even, and `tick % 32` reaches only 16 of the 32 slots. That silently
        // halves the history, and it does so in the direction that costs most --
        // a client on a long link acks a snapshot that has already been evicted,
        // so the server falls back to full snapshots precisely when bandwidth is
        // scarcest.
        SnapshotRecord& r = ring_[next_++ % SNAPSHOT_RING];
        r = SnapshotRecord{};
        r.tick = tick;
        r.valid = true;
        return r;
    }

    /**
     * The record for `tick`, or null if it has aged out.
     *
     * Tick 0 is never a valid baseline -- the protocol uses baseline_tick == 0
     * to mean "full snapshot", so a client that has acked nothing and a client
     * whose baseline expired take the same path.
     *
     * Linear scan: the ring is small, this runs once per client per snapshot,
     * and it removes any coupling between slot layout and tick arithmetic.
     */
    const SnapshotRecord* find(u32 tick) const noexcept {
        if (tick == 0) return nullptr;
        for (u32 i = 0; i < SNAPSHOT_RING; ++i) {
            if (ring_[i].valid && ring_[i].tick == tick) return &ring_[i];
        }
        return nullptr;
    }

private:
    SnapshotRecord ring_[SNAPSHOT_RING]{};
    u32            next_ = 0;
};

/** Which fields of `now` differ from `was`. Zero means "omit this entity". */
inline u32 delta_mask(const EntitySample& was, const EntitySample& now) noexcept {
    // A different generation means a different entity occupying the same slot
    // (protocol R8), so nothing about the old one is a valid baseline. Send
    // everything and let the client destroy-and-respawn.
    if (!was.present || was.generation != now.generation) {
        return proto::ent_field::POS | proto::ent_field::FACING |
               proto::ent_field::ANIMATION_STATE;
    }
    u32 mask = 0;
    if (now.pos != was.pos) mask |= proto::ent_field::POS;
    if (now.facing != was.facing) mask |= proto::ent_field::FACING;
    if (now.animation_state != was.animation_state) mask |= proto::ent_field::ANIMATION_STATE;
    return mask;
}

}  // namespace dnp::server
