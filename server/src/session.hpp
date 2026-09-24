#pragma once
//
// Per-client session state: the input jitter buffer and the bookkeeping
// reconciliation and clock sync depend on.
//
#include <vector>
#include "dnp/proto/messages.hpp"
#include "snapshot_history.hpp"

namespace dnp::server {

using namespace dnp;

// ~2 seconds at 60 Hz. Large enough to absorb any jitter worth absorbing;
// anything beyond it is a client that has lost the plot and should resync.
inline constexpr u32 INPUT_RING = 128;

// Consecutive starved ticks after which a gap is judged unfillable. Half a
// second: a jittery link starves in ones and twos and refills, and anything
// that has starved this long is waiting for ticks that do not exist.
inline constexpr u32 STARVE_GIVE_UP = 30;

class Session {
public:
    u32  id            = 0;

    // Which level this session is IN. A server hosts one simulation per map, so
    // an agent index means nothing without it: slot 3 in MAP01 and slot 3 in
    // MAP02 are two different players.
    u32  level         = 0;
    u32  agent         = 0;
    bool has_agent     = false;
    bool hello_done    = false;

    u32  last_applied_tick   = 0;   // reported as SNAPSHOT.last_input_tick
    u32  ack_snapshot_tick   = 0;

    // Tick of the most recent snapshot sent to this session, so LUA_APPLIED can
    // be scoped to changes this observer has not yet been told about
    // (docs/protocol.md §10.1.3).
    u32  last_snapshot_sent  = 0;

    // Where the FAR half of the entity sweep resumes next tick.
    //
    // A snapshot cannot always carry every entity, and what it drops used to be
    // "everything after the index the datagram filled up on". Agents hold the
    // low indices and props the high ones, so a level with two hundred monsters
    // spent the whole budget on them and the props -- the keys -- were never
    // sent at all. Reported as "the keys stop appearing", and it is not a
    // spawning failure: they exist on the server and no client is ever told.
    //
    // Near entities are sent first and every tick; the rest resume from here
    // and wrap, so the far field takes turns rather than the same tail being
    // dropped forever.
    //
    // THAT NEVER WORKED and the cursor is no longer read. Entities were ordered
    // near-then-far, and within each half by entity INDEX -- so the fifty a
    // player heard about were the fifty with the lowest slot numbers, and once
    // the near half alone overflowed the budget the far half was never reached,
    // cursor or no cursor. Kept as a field only so nothing reading a snapshot
    // has to change; send_snapshots now sorts by distance with an ageing term.
    u32  snapshot_cursor     = 0;

    // The tick each entity was last successfully sent on, or 0 for never.
    //
    // This is the "then time since that entity was last sent" half of
    // docs/protocol.md section 5.4's ordering rule, and without it a strict
    // sort by distance starves the far field exactly as the index order did:
    // an entity past the budget is never sent, so it never becomes stale, so
    // it never rises. Per session, because two players standing in different
    // rooms disagree about what is near.
    u32  last_sent_tick[sim::MAX_ENTITIES]{};
    u32  newest_input_tick   = 0;
    // Bundles discarded as belonging to the world this session just left.
    // Counted rather than dropped in silence: "input stopped working after a
    // map change" is the report this exists to prevent, and a number is what
    // distinguishes "the guard is working" from "the guard is eating input".
    u32  stale_world_bundles = 0;
    u32  stale_world_frames  = 0;

    // Statistics worth watching: a client that starves regularly is either
    // badly connected or has a broken clock-sync loop, and the two look
    // identical from the outside without a counter to separate them.
    u64 starved_ticks = 0;
    u64 duplicate_frames = 0;
    // Times the buffer skipped forward because it had fallen a whole ring
    // behind what it was being sent. Expected once per map change; a stream of
    // them is a client whose clock is not tracking the server at all.
    u64 clock_resyncs = 0;
    // Starved ticks since the last frame was consumed. See take().
    u32 consecutive_starves_ = 0;

    // Panels currently open on this client (docs/protocol.md §11).
    //
    // Kept per session rather than per level, because a panel belongs to the
    // person looking at it. Held whole, not just by id, because an incoming
    // event has to be validated against what was actually offered -- a value
    // clamped to the widget's own range, a select index inside its own option
    // list, and nothing at all for a widget the panel does not contain.
    //
    // A small vector: a client with more than a handful of panels open at once
    // is a script bug, and the cap makes that visible rather than unbounded.
    static constexpr u32 MAX_OPEN_UI = 8;
    std::vector<proto::UiPanel> open_ui;
    // Allocated per session and never reused within one, so an event for a
    // panel that has closed names an id that no longer exists rather than one
    // that has been recycled into a different panel.
    u32 next_ui_id = 1;

    proto::UiPanel* find_ui(u32 ui_id) {
        for (auto& p : open_ui) if (p.ui_id == ui_id) return &p;
        return nullptr;
    }

    // What this client has been sent, for delta encoding (docs/protocol.md 5.3).
    SnapshotHistory history;

    // Bandwidth accounting. Delta compression is only worth its complexity if
    // it actually reduces bytes, so the server measures rather than assumes.
    u64 bytes_sent = 0;
    u64 snapshots_sent = 0;
    u64 entities_omitted = 0;

    // Forget everything about the world this session was watching, without
    // touching who it is or what it has sent us.
    //
    // Called when a session changes level. The snapshot history is a set of
    // baselines describing entities in the map just left: delta-encoding this
    // map's entities against those would produce a valid-looking snapshot full
    // of nonsense, and the input ring would replay presses aimed at a room that
    // no longer exists.
    void forget_history() noexcept {
        history.reset();
        for (u32 i = 0; i < INPUT_RING; ++i) { have_[i] = false; ring_[i] = proto::InputFrame{}; }
        last_applied_tick = 0;
        ack_snapshot_tick = 0;
        last_snapshot_sent = 0;
        snapshot_cursor = 0;
        // Cleared with the rest of the history: a session that moved level and
        // kept these would age every entity against a tick from the world it
        // left, and the ordering would be wrong for as long as the numbers
        // stayed larger than the new level's tick.
        for (u32 i = 0; i < sim::MAX_ENTITIES; ++i) last_sent_tick[i] = 0;
        newest_input_tick = 0;
        next_tick_ = 0;
        started_ = false;
        last_applied_frame_ = proto::InputFrame{};
    }

    void reset() noexcept {
        history.reset();
        bytes_sent = 0;
        snapshots_sent = 0;
        entities_omitted = 0;
        for (u32 i = 0; i < INPUT_RING; ++i) { have_[i] = false; ring_[i] = proto::InputFrame{}; }
        last_applied_tick = 0;
        ack_snapshot_tick = 0;
        last_snapshot_sent = 0;
        snapshot_cursor = 0;
        // Cleared with the rest of the history: a session that moved level and
        // kept these would age every entity against a tick from the world it
        // left, and the ordering would be wrong for as long as the numbers
        // stayed larger than the new level's tick.
        for (u32 i = 0; i < sim::MAX_ENTITIES; ++i) last_sent_tick[i] = 0;
        newest_input_tick = 0;
        next_tick_ = 0;
        started_ = false;
        starved_ticks = 0;
        duplicate_frames = 0;
        last_applied_frame_ = proto::InputFrame{};
    }

    // Store every frame in the bundle. Bundles repeat recent frames on purpose
    // (docs/protocol.md 4.1), so most of these are duplicates and that is the
    // point -- a single lost datagram must not leave a hole in the input stream.
    // Forget every buffered and remembered input.
    //
    // Called when this session changes level. Two things go wrong without it,
    // and together they are a player who walks off on their own:
    //
    // take() repeats the last frame across a gap -- which is right for jitter,
    // because repeating is what the client's own replay assumes. But the last
    // frame before a map change is whatever the player was doing as they
    // touched the exit, and in the new level there is nothing to replace it
    // with: the client resets its clock to the new world's tick, so its frames
    // arrive stamped outside the window this ring is still expecting and are
    // dropped as too old or too far ahead. So the server goes on simulating
    // "walking east" indefinitely while every fresh input is discarded.
    //
    // Clearing `started_` is the load-bearing part: it makes the next frame
    // that arrives re-seed the expected tick, whatever it is.
    void reset_input() noexcept {
        for (u32 i = 0; i < INPUT_RING; ++i) have_[i] = false;
        last_applied_frame_ = proto::InputFrame{};
        started_ = false;
        next_tick_ = 0;
        newest_input_tick = 0;
        consecutive_starves_ = 0;
        last_applied_tick = 0;
    }

    void ingest(const proto::InputBundle& b, u32 world_tick) noexcept {
        ack_snapshot_tick = b.ack_snapshot_tick > ack_snapshot_tick ? b.ack_snapshot_tick
                                                                    : ack_snapshot_tick;
        // INPUT ADDRESSED TO THE WORLD THEY JUST LEFT.
        //
        // A map change reseats the player in a different simulation and the
        // client resets its clock to the new world's tick. But frames sent
        // before it noticed are still in flight, stamped on the old clock -- and
        // whichever arrives first re-seeds this ring, so the server then spends
        // a second crawling through empty ticks toward the client's real
        // position while repeating the last thing the player did before
        // touching the exit. Which is a character walking off on its own.
        //
        // A bundle acknowledges the newest snapshot the client has SEEN, so
        // that is the honest test of which world it was talking about: a
        // snapshot tick THIS world has not reached cannot have come from it.
        //
        // This paragraph described the check for a long time without the check
        // existing, and it went unnoticed because the two worlds' clocks used
        // to run in step -- an old-world frame re-seeded the ring a few ticks
        // off and the buffer absorbed it. Once empty levels stopped ticking,
        // the level being entered was frozen near zero while the one being left
        // was in the thousands, so ONE stale frame re-seeded next_tick_ a
        // thousand ticks ahead and every real frame after it was discarded as
        // too old. The player stopped responding and walked east for ever,
        // which is the exact pair of symptoms the two comments above predict.
        if (b.ack_snapshot_tick > world_tick) { ++stale_world_bundles; return; }

        for (u8 i = 0; i < b.frame_count; ++i) {
            // frames[] is newest first, so frame i is for tick (client_tick - i).
            if (b.client_tick < i) break;
            const u32 t = b.client_tick - i;

            // A client's clock leads the world's by its own latency and a
            // little -- never by a whole buffer. A frame stamped further ahead
            // than the ring is deep is not input for this world, and letting
            // one re-seed the ring after a map change is what makes a player go
            // deaf. Kept as a floor even though levels now share a clock: it
            // costs one compare and it is the difference between a bad frame
            // and a stuck session.
            if (t > world_tick + INPUT_RING) { ++stale_world_frames; continue; }

            if (!started_) { next_tick_ = t; started_ = true; }
            // Too old to matter, or so far ahead it would wrap the ring and
            // overwrite input we have not consumed yet.
            if (t < next_tick_ || t >= next_tick_ + INPUT_RING) continue;

            const u32 slot = t % INPUT_RING;
            if (have_[slot]) { ++duplicate_frames; continue; }
            ring_[slot] = b.frames[i];
            have_[slot] = true;
            if (t > newest_input_tick) newest_input_tick = t;
        }
    }

    // The input to simulate this tick.
    //
    // On a gap, the previous frame is repeated rather than substituting neutral
    // input. Repeating is what the client's own replay assumes, so it keeps the
    // two simulations aligned; zeroing would make the server briefly simulate an
    // input the client never sent and guarantee a misprediction.
    const proto::InputFrame& take() noexcept {
        if (!started_) return last_applied_frame_;

        // CATCH UP when the gap is not going to be filled.
        //
        // On a starve this used to hold `next_tick_` where it was and replay the
        // last frame -- correct for jitter, where the missing frame is still in
        // flight and arrives a moment later. But a gap that will NEVER be filled
        // stalls the buffer permanently: every later frame sits in the ring
        // unread, and the server goes on simulating whatever the player was
        // doing when the gap opened. Forever.
        //
        // Which is what a map change does. The client resets its clock to the
        // new world's tick, so the ticks this buffer is still waiting for do not
        // exist and never will -- and the last thing the player did before
        // touching the exit becomes their permanent input. Reported as
        // "something taking control, and the player character just moves on its
        // own in a chosen direction".
        //
        // The bound is the buffer's own depth: being a whole ring behind the
        // newest thing received is not jitter, it is a clock that moved.
        const u32 slot = next_tick_ % INPUT_RING;
        if (have_[slot]) {
            last_applied_frame_ = ring_[slot];
            have_[slot] = false;
            last_applied_tick = next_tick_;
            ++next_tick_;
            consecutive_starves_ = 0;
            return last_applied_frame_;
        }

        ++starved_ticks;
        ++consecutive_starves_;

        // GIVE UP ON A GAP THAT IS NEVER GOING TO BE FILLED.
        //
        // Waiting is right for jitter: the missing frame is in flight and
        // arrives a moment later, and repeating until it does is what the
        // client's own replay assumes. It is wrong for a gap that does not
        // exist -- a map change resets the client's clock, so the ticks this
        // buffer is waiting for will never be sent, and without this it waits
        // there forever replaying whatever the player was doing when the gap
        // opened. Which is a character walking off on its own.
        //
        // The discriminator is HOW LONG IT HAS STARVED, not how far ahead the
        // newest frame is. Two earlier versions used the gap and both were
        // wrong: a client whose clock merely runs fast leads by more and more
        // without anything being broken, and skipping on that threw away input
        // sitting in the ring waiting its turn -- attack.test.js swung six
        // times and the server saw none of them, one run in three, and the
        // netsim matrix mispredicted at 150 ms where it had been exact.
        //
        // Half a second of continuous starvation is not jitter. A 300 ms link
        // starves in ones and twos and refills; a gap that will never be filled
        // starves every tick from now on.
        if (consecutive_starves_ > STARVE_GIVE_UP && newest_input_tick > next_tick_) {
            next_tick_ = newest_input_tick;
            // Nothing is carried across: the frames either side of a
            // discontinuity are unrelated, and repeating the old one into the
            // new world is the bug itself.
            last_applied_frame_ = proto::InputFrame{};
            consecutive_starves_ = 0;
            ++clock_resyncs;
        }
        return last_applied_frame_;
    }

    // Ticks of input in hand. Drives the client's clock-sync loop
    // (docs/protocol.md 10.1): positive means buffered and healthy, zero or
    // negative means the server is starving and the client should run further
    // ahead.
    i8 margin() const noexcept {
        if (!started_) return 0;
        const i64 m = static_cast<i64>(newest_input_tick) - static_cast<i64>(next_tick_) + 1;
        if (m > 127) return 127;
        if (m < -128) return -128;
        return static_cast<i8>(m);
    }

private:
    proto::InputFrame ring_[INPUT_RING]{};
    bool              have_[INPUT_RING]{};
    proto::InputFrame last_applied_frame_{};
    u32               next_tick_ = 0;
    bool              started_ = false;
};

}  // namespace dnp::server
