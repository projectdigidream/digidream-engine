#pragma once
//
// WHERE EVERY BODY WAS, a second back (docs/WEAPON_PREDICTION.md, phase E).
//
// A player draws everybody else a little in the past -- the snapshot it last
// applied, less its interpolation delay -- and aims at what it draws. Each
// input frame says which tick that was (InputFrame::view).
// So a shot is judged against the bodies WHERE THAT PLAYER SAW THEM: the
// server keeps their positions for the last DEPTH ticks, and while the core
// answers one player's shot and auto-aim (World::set_query_poses) it reads
// them from here. Only those two questions: the world is never rewound, and a
// door that closed in between still stops the shot.
//
// Recorded after every step, keyed by the world's tick then -- which is the
// tick a snapshot written from that state carries, so "the snapshot the
// client applied" names a slot here directly.
//
#include <vector>

#include "dnp/sim/world.hpp"

namespace dnp::server {

struct PoseHistory {
    // About half a second at 60 Hz: past any rewind the cap below allows.
    static constexpr u32 DEPTH = 32;

    struct Slot {
        u32 tick = 0;
        bool used = false;
        std::vector<fx3> pos;
        std::vector<u8>  live;   // active and solid then
    };
    Slot slots[DEPTH];

    void record(const sim::World& w) {
        Slot& s = slots[w.tick() % DEPTH];
        const u32 n = w.agent_high_water();
        s.tick = w.tick();
        s.used = true;
        s.pos.resize(n);
        s.live.resize(n);
        for (u32 j = 0; j < n; ++j) {
            const sim::AgentState& a = w.agent(j);
            s.pos[j] = a.pos;
            s.live[j] = (a.active && a.solid) ? 1 : 0;
        }
    }

    const Slot* at(u32 tick) const {
        const Slot& s = slots[tick % DEPTH];
        return (s.used && s.tick == tick) ? &s : nullptr;
    }

    void clear() { for (Slot& s : slots) s.used = false; }
};

// How far a shot may be judged in the past, in ticks: 15 is 250 ms. Beyond it
// a player on a slow link would be shooting at where somebody was a moment
// ago while that somebody has long since stepped round a corner -- which is
// the complaint lag compensation trades for, capped so it stays small.
inline constexpr u32 MAX_REWIND_TICKS = 15;

}  // namespace dnp::server
