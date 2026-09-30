#pragma once
//
// WHAT A CLIENT HOLDS, AND WHAT TO TELL IT SO THAT IT HOLDS WHAT IT SHOULD.
//
// The contract, stated once and followed on both sides (docs/protocol.md 8.5):
//
//   A client HOLDS a level from the moment the server starts streaming its map
//   to it -- as its home level after a WELCOME, or as a neighbour after a
//   NEIGHBOUR_MAP -- until the server sends NEIGHBOUR_DROP for it. Nothing else
//   ends a hold: not a crossing, not a map change, not the client deciding it
//   has no use for it.
//
// "Held" is bytes, not a mesh. Whether the client has finished building
// something to draw is its own business and this never guesses at it. The
// channel is reliable and ordered, so a map the server has started to send is
// a map the client will have in full before anything sent after it.
//
// That rule is the entire fix for a family of crossing bugs, every one of which
// was the two sides disagreeing about what the other had: a level set aside by
// the crossing before this one, a map still being built, a copy filed under
// the wrong index. With the rule there is nothing to disagree about, and the
// server can say "the map does not follow" and be right.
//
// PURE, like the rest of this directory: level indices and offsets in, messages
// out. Tested in server/tests/test_neighbours.cpp without a server.
//
#include <algorithm>
#include <vector>

#include "dnp/math/fixed.hpp"

namespace dnp::server::world {

using namespace dnp;

// One level a client holds besides the one it is standing in.
//
// `placed` false means the client holds the bytes but has not been told where
// to draw them from its current home -- which is the level it just LEFT: its
// map was sent as a home map, with no offset, and its offset from the new home
// has not been said yet. The next diff states it.
struct HeldLevel {
    u32  level  = 0;
    i64  dx     = 0;   // raw fx: the level's origin minus the home's
    i64  dy     = 0;
    bool placed = false;
};

// One level a client SHOULD hold, with where it sits relative to its home.
struct WantedLevel {
    u32 level = 0;
    i64 dx    = 0;
    i64 dy    = 0;
};

enum class NeighbourSay : u8 { Drop, Move, Map };

struct NeighbourMessage {
    NeighbourSay say   = NeighbourSay::Drop;
    u32          level = 0;
    i64          dx    = 0;
    i64          dy    = 0;
};

// THE LEVEL CHANGE, as far as holding goes.
//
// The level left becomes an ordinary held level -- the client has its bytes,
// they were its home map -- with no offset yet. The level entered stops being
// a neighbour, because nothing is its own neighbour.
//
// Returns whether the level entered was already held, which is exactly whether
// its map has to follow the WELCOME: held means the client has the bytes, by
// the rule above, so sending them again is a second copy of something it has.
//
// `from == to` is a respawn in the same level: nothing is held that was not,
// and the map is held already.
inline bool rehome(std::vector<HeldLevel>& held, bool had_home, u32 from, u32 to) {
    if (had_home && from == to) return true;
    bool was_held = false;
    for (size_t k = held.size(); k-- > 0;) {
        if (held[k].level != to) continue;
        held.erase(held.begin() + static_cast<long>(k));
        was_held = true;
    }
    if (had_home) {
        const bool listed = std::any_of(held.begin(), held.end(),
                                        [from](const HeldLevel& h) { return h.level == from; });
        if (!listed) held.push_back(HeldLevel{from, 0, 0, false});
    }
    return was_held;
}

// THE DIFFERENCE between what is held and what should be, as the messages that
// close it, applied to `held` as they are produced.
//
// Drops first, in the order held, so a client that is short of memory frees
// before it is asked to fill. Then, in the order wanted: nothing for a level
// held exactly where it was last said to be, a MOVE for one held elsewhere or
// not yet placed, a MAP for one not held.
//
// A level may be wanted that is also the home: it is ignored. The caller
// excludes it, and a second line of defence costs one compare.
inline std::vector<NeighbourMessage> diff_neighbours(std::vector<HeldLevel>& held,
                                                     const std::vector<WantedLevel>& want,
                                                     u32 home) {
    std::vector<NeighbourMessage> out;
    const auto wanted = [&](u32 li) {
        return li != home && std::any_of(want.begin(), want.end(),
                                         [li](const WantedLevel& w) { return w.level == li; });
    };
    for (size_t k = 0; k < held.size();) {
        if (wanted(held[k].level)) { ++k; continue; }
        out.push_back(NeighbourMessage{NeighbourSay::Drop, held[k].level, 0, 0});
        held.erase(held.begin() + static_cast<long>(k));
    }
    for (const WantedLevel& w : want) {
        if (w.level == home) continue;
        auto it = std::find_if(held.begin(), held.end(),
                               [&w](const HeldLevel& h) { return h.level == w.level; });
        if (it == held.end()) {
            out.push_back(NeighbourMessage{NeighbourSay::Map, w.level, w.dx, w.dy});
            held.push_back(HeldLevel{w.level, w.dx, w.dy, true});
            continue;
        }
        if (it->placed && it->dx == w.dx && it->dy == w.dy) continue;
        out.push_back(NeighbourMessage{NeighbourSay::Move, w.level, w.dx, w.dy});
        it->dx = w.dx;
        it->dy = w.dy;
        it->placed = true;
    }
    return out;
}

}  // namespace dnp::server::world
