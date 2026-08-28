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
    u32  newest_input_tick   = 0;

    // Statistics worth watching: a client that starves regularly is either
    // badly connected or has a broken clock-sync loop, and the two look
    // identical from the outside without a counter to separate them.
    u64 starved_ticks = 0;
    u64 duplicate_frames = 0;
    // Times the buffer skipped forward because it had fallen a whole ring
    // behind what it was being sent. Expected once per map change; a stream of
    // them is a client whose clock is not tracking the server at all.
    u64 clock_resyncs = 0;

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
        last_applied_tick = 0;
    }

    void ingest(const proto::InputBundle& b) noexcept {
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
        // that is the honest test of which world it was talking about.


        for (u8 i = 0; i < b.frame_count; ++i) {
            // frames[] is newest first, so frame i is for tick (client_tick - i).
            if (b.client_tick < i) break;
            const u32 t = b.client_tick - i;

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
        if (newest_input_tick > next_tick_ + INPUT_RING / 2) {
            next_tick_ = newest_input_tick - INPUT_RING / 4;
            // Nothing is carried across the jump. The frames either side of a
            // discontinuity are unrelated, and repeating the old one into the
            // new world is the bug itself.
            last_applied_frame_ = proto::InputFrame{};
            ++clock_resyncs;
        }

        const u32 slot = next_tick_ % INPUT_RING;
        if (have_[slot]) {
            last_applied_frame_ = ring_[slot];
            have_[slot] = false;
            last_applied_tick = next_tick_;
            ++next_tick_;
        } else {
            ++starved_ticks;
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
