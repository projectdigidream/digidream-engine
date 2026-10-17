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
#include <algorithm>
#include <vector>

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
    // Which weapon step its running swing is (proto::ent_field::ATTACK_STEP);
    // weapon 0xFF when none is, or the swing is its parameters'. Up here, in
    // the padding before `pos`, so the ring a session holds does not grow.
    u8   attack_weapon   = 0xFF;
    u8   attack_phase    = 0xFF;
    u8   attack_step     = 0;
    // The particle kind following it (proto::ent_field::TRAIL), 0 none. In
    // the same padding.
    u16  trail           = 0;
    fx3  pos{};
    brad facing{};
    u16  animation_state = 0;
    // What the entity IS, for anything that is not a player. Sent once, with
    // the first snapshot that carries the entity, because a key does not become
    // a different key -- and a client that missed it would have a position with
    // nothing to draw at it.
    u32  display_id      = 0;
    u16  avatar          = 0;
    // Whether other bodies collide with this one. Replicated because the client
    // predicts against it -- see proto::ent_field::SOLID.
    bool solid           = true;
    // Ticks left in its swing (proto::ent_field::ATTACK); 0 when not swinging.
    u8   attack_left     = 0;
    // World::teleports: a count that moved since the baseline is a JUMP, sent
    // as proto::ent_field::TELEPORTED beside the position.
    u8   teleports       = 0;
};

struct SnapshotRecord {
    u32  tick  = 0;
    bool valid = false;
    // Over the whole entity space, not just the agents: props live in the
    // slots above MAX_AGENTS and are delta-encoded by exactly the same rules.
    EntitySample entities[sim::MAX_ENTITIES]{};
};

// ON THE HEAP. A record holds a sample per possible entity, and the ring is
// thirty-two of them: about 12.6 MB at 8192 entities. Held inline, a Session --
// which the server builds as a local before storing it -- was more than the
// main thread's 8 MB stack, and the server died the moment anybody connected.
// Records are cleared in place for the same reason: assigning a fresh one
// builds a 400 KB temporary on the stack first.
class SnapshotHistory {
public:
    SnapshotHistory() : ring_(SNAPSHOT_RING) {}

    void reset() noexcept {
        for (SnapshotRecord& r : ring_) clear(r);
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
        clear(r);
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
    static void clear(SnapshotRecord& r) noexcept {
        r.tick = 0;
        r.valid = false;
        std::fill(std::begin(r.entities), std::end(r.entities), EntitySample{});
    }
    std::vector<SnapshotRecord> ring_;
    u32                         next_ = 0;
};

/** Which fields of `now` differ from `was`. Zero means "omit this entity". */
inline u32 delta_mask(const EntitySample& was, const EntitySample& now) noexcept {
    // A different generation means a different entity occupying the same slot
    // (protocol R8), so nothing about the old one is a valid baseline. Send
    // everything and let the client destroy-and-respawn.
    if (!was.present || was.generation != now.generation) {
        u32 full = proto::ent_field::POS | proto::ent_field::FACING |
                   proto::ent_field::ANIMATION_STATE;
        // Only when there is something to say. Every agent would otherwise
        // carry a display_id of zero on every respawn, which costs four bytes
        // to tell the client what it already assumes.
        if (now.display_id != 0) full |= proto::ent_field::DISPLAY_ID;
        // Same again: avatar 0 is the default a client already assumes, so it is
        // only worth two bytes when a script has actually chosen something.
        if (now.avatar != 0) full |= proto::ent_field::AVATAR;
        // Same reasoning: solid is the default, so it is only worth a byte when
        // it is not. A body that spawns already dead does not exist.
        if (!now.solid) full |= proto::ent_field::SOLID;
        if (now.attack_left != 0) full |= proto::ent_field::ATTACK;
        if (now.attack_weapon != 0xFF) full |= proto::ent_field::ATTACK_STEP;
        if (now.trail != 0) full |= proto::ent_field::TRAIL;
        return full;
    }
    u32 mask = 0;
    if (now.pos != was.pos) mask |= proto::ent_field::POS;
    if (now.facing != was.facing) mask |= proto::ent_field::FACING;
    if (now.animation_state != was.animation_state) mask |= proto::ent_field::ANIMATION_STATE;
    if (now.display_id != was.display_id) mask |= proto::ent_field::DISPLAY_ID;
    // Delta-masked rather than sent once, because an avatar CAN change -- a class
    // swap, a costume -- and a field sent only on appearance would leave everyone
    // else looking at the old one forever.
    if (now.avatar != was.avatar) mask |= proto::ent_field::AVATAR;
    if (now.solid != was.solid) mask |= proto::ent_field::SOLID;
    if (now.attack_left != was.attack_left) mask |= proto::ent_field::ATTACK;
    if (now.attack_weapon != was.attack_weapon || now.attack_phase != was.attack_phase ||
        now.attack_step != was.attack_step) mask |= proto::ent_field::ATTACK_STEP;
    if (now.trail != was.trail) mask |= proto::ent_field::TRAIL;
    // Counted rather than flagged, so it holds against any baseline: every
    // snapshot whose baseline is older than the jump says so, until one is
    // acknowledged that is newer.
    if (now.teleports != was.teleports) mask |= proto::ent_field::TELEPORTED | proto::ent_field::POS;
    return mask;
}

}  // namespace dnp::server
