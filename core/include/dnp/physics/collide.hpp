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
#include "dnp/math/trig.hpp"
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
//
// `feet_z` and `body_height` decide WHICH sector, where footprints overlap: a
// bridge over a path shares floor area with it, and which floor applies depends
// on where the body is, not on which sector is smaller or came first.
SurfaceQuery query_surface(const geom::MapData& map, fx2 p, i32 hint,
                           fx feet_z, fx body_height) noexcept;

// Can a body of `body_height`, whose feet are at `feet_z`, occupy sector `s`?
//
// The step-up threshold and the headroom requirement, in one predicate. It is
// separate from side_is_passable() because the sweep of §3.1.3 asks the same
// question in the opposite direction: a wall moving toward an agent is solid
// exactly when the agent could not stand in the sector behind it, and that is a
// property of the destination sector, not of the side that happens to name it.
bool sector_admits(const geom::MapData& map, u32 s, fx feet_z, fx body_height) noexcept;

// Is the boundary between `from` and the side's neighbour passable for an agent
// standing at height `feet_z` with the given body height?
//
// This is the single place the step-up rule is decided, so "walkable" cannot
// drift between the mover and anything else that asks.
bool side_is_passable(const geom::MapData& map, u32 side_index, u32 from,
                      fx feet_z, fx body_height) noexcept;

// Does this sector have an open edge -- somewhere an entity may leave the map?
//
// Asked by move() before it will let anything end a tick outside the world, so
// a floating platform lets you fall off it and a closed room still cannot.
bool sector_has_ledge(const geom::MapData& map, i32 s) noexcept;

struct SweepContact {
    bool contacted = false;
    fx2  escape{};      // total displacement applied to clear the geometry
};

// Push `p` out of every impassable side of the sector it is standing in.
//
// This is RESTING CONTACT: the agent is inside `s`, and a side is solid when it
// cannot walk out through it. For a sector that has moved into an agent
// standing elsewhere, use sweep_out_of_sector() instead -- see the note there.
//
// `vel`, when non-null, has its into-wall component removed per contact -- the
// wall-slide rule.
SweepContact push_out_of_sector(const geom::MapData& map, u32 s, fx2& p, fx3* vel,
                                fx radius, fx feet_z, fx body_height) noexcept;

// Keep a cylinder out of the solid volume of a sector that has MOVED into it
// (design §3.1.3), and report the displacement that took.
//
// Deliberately a separate function from push_out_of_sector, because the two ask
// opposite questions. Resting contact asks whether the agent may leave the
// sector it is standing in through a given wall. A sweep asks whether the agent
// may be where the moving sector now is -- which is a property of that sector's
// own floor and ceiling, and stays the right question whether the wall has
// reached the agent or already passed over it.
SweepContact sweep_out_of_sector(const geom::MapData& map, u32 sector, fx2& p,
                                 fx radius, fx feet_z, fx body_height) noexcept;

// Keep a cylinder out of the SLAB beneath a sector's floor.
//
// Within a sector's footprint, everything below its floor is solid -- that is
// what a floating platform IS. Nothing modelled it while every map was a sealed
// box, because there was no "below the floor" to be in. With open edges there
// is, and without this an entity falling past a platform drifts straight
// through its side and out the far face.
//
// It also removes a question rather than answering it: an entity can no longer
// arrive underneath a platform, so nothing has to decide whether a floor
// catches something approaching from below.
SweepContact push_out_of_slab(const geom::MapData& map, u32 sector, fx2& p,
                              fx radius) noexcept;

// Resolve a cylinder against EVERY slab it overlaps.
//
// Not just the sector its centre is in: a body overlaps a platform's edge well
// before its centre crosses the footprint, and a test that starts from "which
// sector am I in" lets it sink a full radius into the edge before reacting.
//
// A slab blocks the body exactly when the body could not STAND in the sector
// above it, which is sector_admits -- so a step up is unobstructed and a tower
// is solid, from one rule rather than two.
SweepContact resolve_slabs(const geom::MapData& map, fx2& p, fx radius,
                           fx feet_z, fx body_height) noexcept;

// Nearest side within `radius` of `p` carrying `flag`, or -1.
//
// Used by climbing (CLIMBABLE) and by direct grapple attachment (GRAPPLE).
//
// `out_normal` faces INWARD -- from the wall toward the agent -- which is the
// direction every caller actually wants: it is the way a wall jump pushes off,
// and the surface orientation a climbing agent aligns to. side_normal() returns
// the outward normal, and handing that straight back made wall jumps drive
// into the wall rather than away from it.
i32 find_side_with_flag(const geom::MapData& map, i32 sector, fx2 p, fx radius,
                        u8 flag, fx2& out_normal) noexcept;

// The side an entity is reaching for, or -1.
//
// The nearest side of its own sector within INTERACT_REACH that lies in front
// of it. Deliberately not a ray cast: a ray through a doorway's exact centre is
// a harsher test than a player expects, and "the nearest thing you are facing"
// is both kinder and exactly as deterministic.
//
// Native and fixed, like every other query a client predicts against. What
// HAPPENS when a side is worked is Lua's -- the engine reports that an entity
// reached for one and stops there, the same division §3.1 draws for crushing.
i32 find_interact_side(const geom::MapData& map, i32 sector, fx2 p, brad facing,
                       fx reach, fx feet_z, fx body_height) noexcept;

struct RayHit {
    bool hit = false;
    fx2  point{};
    fx2  normal{};
    fx   distance{};
    i32  side = -1;

    // Stopped by geometry before reaching `max_dist`.
    //
    // Distinct from `hit`, which means "found a side carrying the flag you
    // asked for". A ray that runs into a wall satisfies neither -- and for the
    // grapple, which asks "is there something attachable within reach", that is
    // the right answer. But "did a wall stop this" is a different question, and
    // every caller that asks it -- line of sight, a bullet, a projectile --
    // could not tell being blocked from running out of range. They all reported
    // a clear shot through solid rock.
    //
    // When this is set, `point`, `distance` and `side` describe the OBSTRUCTION.
    bool blocked = false;

    // WHY it was blocked, when what stopped it was the HEIGHT rather than the
    // wall.
    //
    // Zero is a wall -- one-sided, BLOCKING, or a shut door stored as a sector
    // with its ceiling on its floor -- and no choice of aim gets past one. The
    // other two are facts about the AIM and not about the map: -1 says the ray
    // passed below the neighbour's floor and +1 that it passed above its
    // ceiling, and in both cases the same boundary is open to a ray aimed the
    // other way.
    //
    // Sight is what reads it. A single line between two eyes cannot see a body
    // whose head is over a ledge your eyeline runs under, and knowing which way
    // it failed is what lets it retry once instead of answering no.
    i8 height_block = 0;
};

// Cast a ray across the sectors it passes through, stopping at the first side
// carrying `flag` (or at any blocking side, which occludes).
//
// Reuses the same swept segment test CCD is built on (design §7.1.1: the ranged
// grapple "reuses the same swept/ray-based approach as CCD"), so aiming and
// collision cannot disagree about what is in the way.
// `z`, when given, is the HEIGHT the ray travels at, and a boundary occludes
// unless the sector beyond it is open at that height.
//
// Without it the cast is purely two-dimensional, and a two-sided line is
// transparent no matter what is behind it: a monster shot across a room, over a
// ledge whose floor is above its head, through a sector whose ceiling is below
// its feet, and out the far side of the map. In a converted Doom level almost
// every wall that is not a plain one-sided linedef is exactly that -- a step, a
// balcony, a shut door stored as a sector with its ceiling on its floor -- so
// "does it have a neighbour" was answering a much weaker question than sight,
// bullets and projectiles were asking of it.
//
// NO_HEIGHT keeps the flat behaviour for callers that genuinely want it and for
// the golden traces that predate this.
inline constexpr fx RAY_NO_HEIGHT = fx::from_raw(INT64_MIN);

// `z_end`, when given, makes the ray SLOPE: it starts at `z` and arrives at
// `z_end`, and each boundary is tested against the height the ray is at where
// it crosses rather than against one height for the whole cast.
//
// Sight needs this and a flat ray cannot express it. A monster standing on a
// step looking down at somebody was asking "is there a clear horizontal line at
// MY eye height", which is a different line from the one to its target: the
// horizontal one runs into the far room's ceiling, or over the target's head
// and into the wall behind it. The monster then decided it could not see a
// player standing in the open a few units below, lost its target, reacquired
// it, and stood on the step doing that -- reported as an enemy stuck on a step
// whenever the player was lower than it.
//
// Left out, the ray is flat and every existing caller behaves exactly as it did.
RayHit raycast(const geom::MapData& map, i32 sector, fx2 from, fx2 dir, fx max_dist,
               u8 flag, fx z = RAY_NO_HEIGHT, fx z_end = RAY_NO_HEIGHT) noexcept;

// CAN A BODY AT `eye` SEE A BODY STANDING AT `target` BETWEEN `low` and `high`?
//
// A body is a COLUMN, not a point, and that is the whole of what this adds over
// a single raycast. Sight used to be one line drawn between two eyes, which is
// strictly harsher than what a player sees: stand a few units back from a
// ledge and a monster below can watch your head over the edge long before your
// waist comes into view. The single line said no, so the monster lost its
// target, went back to looking, found you again a moment later and lost you
// again -- reported as monsters forgetting the player whenever he was higher or
// lower than they were.
//
// Doom answers this by narrowing an aperture as it walks, which is exact and is
// a second walk to write and maintain beside `raycast`'s. This instead casts at
// the middle and, if the HEIGHT was what stopped it, casts once more at
// whichever end of the column the failure points towards. One extra walk, only
// on the rays that were about to answer no, and none at all for a ray stopped
// by an actual wall.
//
// ponytail: two casts, not an aperture. A body visible only through a slot
// that neither its middle nor that one end lines up with reads as hidden.
// Doom's slope-interval walk is the upgrade if that ever shows up in a level.
bool can_see(const geom::MapData& map, i32 from_sector, fx3 eye,
             fx2 target, fx low, fx high) noexcept;

}  // namespace dnp::physics
