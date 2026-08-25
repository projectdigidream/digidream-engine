#pragma once
//
// Sector collision resolution (design §7.1). Native, fixed, non-scriptable.
//
// Everything here is part of the universal contract: a forked server may not
// change any of it, because a standard client predicts with these exact rules.
// That is why the step threshold is a constant in sim/constants.hpp rather than
// a parameter -- a server that made slopes steeper would break prediction for
// every client built against the standard.
//
// The four rules, and why each exists:
//
//   Step up    -- a height difference at or under STEP_HEIGHT is climbed
//                 without interrupting horizontal movement, so curbs, stairs
//                 and sector-edge seams do not require a jump.
//   Step down  -- the symmetric case. Without it a grounded agent running down
//                 stairs leaves the ground for a few frames per step and the
//                 movement feels floaty and broken.
//   Wall slide -- velocity into a wall is removed, velocity along it survives.
//                 Zeroing the whole vector on contact is what makes movement
//                 feel sticky in corners.
//   CCD        -- above a displacement threshold the move is swept rather than
//                 sampled, so a fast projectile or a hard knockback cannot pass
//                 through a thin wall by being on both sides at consecutive
//                 ticks and inside it at neither.
//
#include "dnp/geom/map.hpp"
#include "dnp/sim/constants.hpp"

namespace dnp::physics {

using namespace dnp;

struct MoveInput {
    fx3 pos{};
    fx3 vel{};
    fx  radius = fx::from_frac(1, 2);
    fx  height = fx::from_int(2);
    i32 sector = geom::NO_SECTOR;   // hint; recomputed if stale
    bool grounded = false;
};

struct MoveResult {
    fx3  pos{};
    fx3  vel{};
    i32  sector = geom::NO_SECTOR;
    bool grounded = false;
    bool hit_wall = false;
    bool hit_ceiling = false;
    bool stepped_up = false;
    bool stepped_down = false;
    bool swept = false;          // CCD path was taken
    u32  iterations = 0;         // slide iterations consumed
};

// Resolve one tick of movement for a vertical-cylinder agent.
//
// Horizontal first, then vertical. The order is not arbitrary: resolving
// vertically first would let an agent that is about to step up be pushed to the
// floor height it has not reached yet, and it would turn every stair into a
// collision against the riser.
MoveResult move(const geom::MapData& map, const MoveInput& in, fx dt) noexcept;

// Floor and ceiling of the sector at `p`, or the given defaults when outside
// the map. Exposed because the ability code (hover, flight, grapple) needs to
// ask about the surface below without performing a move.
struct SurfaceQuery {
    bool in_map = false;
    i32  sector = geom::NO_SECTOR;
    fx   floor_z{};
    fx   ceil_z{};
};
SurfaceQuery query_surface(const geom::MapData& map, fx2 p, i32 hint) noexcept;

// Is the boundary between `from` and the side's neighbour passable for an agent
// standing at height `feet_z` with the given body height?
//
// This is the single place the step-up rule is decided, so "walkable" cannot
// drift between the mover and anything else that asks.
bool side_is_passable(const geom::MapData& map, u32 side_index, u32 from,
                      fx feet_z, fx body_height) noexcept;

}  // namespace dnp::physics
