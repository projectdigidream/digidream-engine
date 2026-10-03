#pragma once
//
// Universal simulation constants.
//
// These are part of the fixed contract of design section 2, NOT per-server
// tunables. A forked server may change player limits, snapshot rate and all
// gameplay Lua; it may not change anything here, because a standard client
// predicts with these values and a server that used different ones would break
// prediction for every client built against the standard.
//
// Anything a server IS allowed to vary belongs in its own configuration, not in
// this file.
//
#include "dnp/math/fixed.hpp"

namespace dnp::sim {

// Fixed simulation rate (docs/physics-contract.md rule 3). Rendering may run at
// any frame rate and interpolate; the simulation step is always exactly this.
inline constexpr u32 TICK_RATE_HZ = 60;

// How many bodies one simulation holds -- players and monsters together, since
// a monster is an agent and runs the player's mover.
//
// Here rather than in world.hpp because the physics layer needs it too: the
// swing's "already hit" mask is one bit per agent slot, and a mask sized by
// coincidence rather than by this constant is a bug waiting for the day the
// number changes.
//
// 64 was a placeholder from before anything scripted spawned bodies, and it
// silently truncated real levels -- the largest converted Doom map places 166
// monsters, so two thirds of it did not exist and the only sign of that was one
// line saying "N did not fit".
//
// 384 WAS ITSELF A CEILING SOMEBODY HIT. Players and monsters share this space,
// because a monster is an agent that runs the player's mover -- so a level with
// 384 monsters has no slot left for a PLAYER. The session gets no agent,
// maps/scripts/mobs.lua sees `#players == 0` and puts the level to sleep, and
// the tick gets cheaper while the game becomes unjoinable.
// tools/mobperf/run.js refuses to report a number in that state for exactly
// that reason; it is where this figure came from.
//
// Raising it is not free and the costs are named where they are paid:
// MoveState::attack_hit_agents is one bit per slot inside EVERY agent, the
// mirror band in world.hpp is carved out of the prop range, and a session's
// snapshot history is MAX_ENTITIES samples deep times SNAPSHOT_RING. Measure
// resident memory when changing it, not just the tick.
// 2048 since a world holds several levels at once (geom::MapStorage::
// add_chunk): Doom II averages 119 monsters a level, so nine average levels --
// a 3x3 neighbourhood -- are ~1070 before anybody joins, and the nine busiest
// 1759. Measured at 4096 and not taken: the per-agent hit bitset makes World
// grow with the square (4.9 MB), and a session's snapshot history reached
// 13 MB, which a server with a hundred players cannot pay.
inline constexpr u32 MAX_AGENTS = 2048;

// WHERE A FIRST-PERSON BODY'S EYE IS, as a fraction of its height: DOOM's own,
// a 41-unit view height on a 56-unit player. The camera sits there and a
// first-person shot leaves from there, so the ray goes where the crosshair is
// rather than a third of a body below it.
inline constexpr fx EYE_FRACTION = fx::from_frac(41, 56);

// How many 64-bit words a swing's record of the props it has hit takes: one
// bit per spawnable prop slot (World asserts it covers SPAWNABLE_PROPS).
inline constexpr u32 ATTACK_HIT_PROP_WORDS = 64;

// Written as an exact rational rather than a decimal literal, so that changing
// the rate cannot introduce a rounding difference that only shows up as drift.
inline constexpr fx DT = fx::from_frac(1, TICK_RATE_HZ);

// Step-up / step-down threshold (design 7.1). One constant serves both
// directions by construction, so "walkable" is symmetric and there is no second
// value to keep in sync. Slope walkability is a consequence of this applied
// incrementally, not a separate parameter.
inline constexpr fx STEP_HEIGHT = fx::from_frac(1, 2);

// How far a body that did not move may be pushed out of another body, per tick.
//
// A CONSTANT and part of the universal contract, like STEP_HEIGHT, because a
// client predicts against it: a server that unstuck bodies faster would put
// every client's prediction slightly behind its own.
//
// It exists so that two bodies already inside each other -- spawned on the same
// tag, or one placed on top of the other by a script -- can separate at all,
// while a body being walked into is not shoved: the resolution is otherwise
// capped at how far the body itself travelled, so a standing body's cap is
// zero. A twentieth of a unit a tick is three units a second, which unsticks a
// spawn overlap in well under a second and is far too slow to push anyone
// anywhere they did not intend to go.
inline constexpr fx DEPENETRATE_PER_TICK = fx::from_frac(1, 20);

// Default gravity. Per-entity `gravity_scale` (design 7.2) multiplies this;
// Lua tunes the scale, never the base.
inline constexpr fx GRAVITY = fx::from_frac(-1600, 100);

// Above this per-tick displacement, collision uses a swept test instead of a
// discrete one (design 7.1, CCD). Below it, the cheaper check is used.
inline constexpr fx CCD_DISPLACEMENT_THRESHOLD = fx::from_frac(1, 4);

// How far an entity can reach to work a switch, a door or a lift.
//
// A CONSTANT, not a parameter, for the same reason STEP_HEIGHT is: a client
// predicts against these rules, and a server that let entities reach further
// would make every client's idea of what it can touch wrong. 1.5 units is
// about 72 at a converted Doom map's scale, against Doom's own 64.
inline constexpr fx INTERACT_REACH = fx::from_frac(3, 2);

// How far a close-range attack reaches, measured to the SURFACE of what it
// hits, so a big target is hittable from further away.
//
// A CONSTANT for the same reason INTERACT_REACH is: a client predicts against
// these rules. What a swing DOES -- damage, knockback, whether it hits at all --
// is a script's decision, and a script that wants a longer weapon expresses it
// by widening what it accepts rather than by changing what the engine reports.
// 2.0 units is about 96 at a converted Doom map's scale, against Doom's own
// 64-unit melee.
inline constexpr fx ATTACK_REACH = fx::from_int(2);

// The half-angle of the swing, as a fraction of a turn. 1/8 of a turn is 45
// degrees either side: a 90-degree arc, which is wide enough that hitting what
// you are looking at does not require precision the camera cannot offer.
inline constexpr u32 ATTACK_HALF_ARC_BRAD = 0xFFFFFFFFu / 8;

}  // namespace dnp::sim
