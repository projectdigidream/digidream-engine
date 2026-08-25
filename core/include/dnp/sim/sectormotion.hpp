#pragma once
//
// Sector motion (design §3.1.2).
//
// The split between the two kinds is not a categorisation exercise -- it is
// what decides whether a client can predict an entity riding the sector:
//
//   Parametric        motion is a pure function of the tick and a small
//                     parameter set. Replicated ONCE, then both sides compute
//                     the same position for any tick with the same maths. An
//                     anchored entity predicts normally.
//
//   ScriptedDiscrete  an arbitrary Lua decision per tick. The client cannot
//                     know in advance what a callback will choose, so it cannot
//                     compute the sector's future position at all, and must
//                     fall back to anchor-relative prediction (§3.2.5).
//
// Both are Lua-TRIGGERED. The distinction is the shape of the motion, not who
// started it: Lua may kick off a parametric slide from A to B over three
// seconds, and that runs predictably from then on.
//
#include "dnp/geom/mapstore.hpp"
#include "dnp/sim/constants.hpp"
#include "dnp/sim/state_hash.hpp"

namespace dnp::sim {

using namespace dnp;

inline constexpr u32 MAX_MOVING_SECTORS = 64;

enum class MotionType : u8 {
    Static = 0,
    Parametric = 1,
    ScriptedDiscrete = 2,
};

enum class LoopMode : u8 {
    Once = 0,       // holds at the far end
    Loop = 1,       // snaps back and repeats
    PingPong = 2,   // reverses
};

// Which surfaces the vertical part of the offset moves.
//
// A platform moves as a volume; a door or a crusher moves one surface and
// leaves the other where it is. Without this distinction the gap between floor
// and ceiling is constant for every mover in the engine, and §3.1's crushing
// has nothing that can ever cause it. Horizontal motion always moves the whole
// footprint -- a sector cannot have its floor in one place and its ceiling in
// another.
enum class MotionTarget : u8 {
    Both = 0,      // the sector moves as a body: a lift, a platform
    Floor = 1,     // Doom's lift: the floor rises into the room above it
    Ceiling = 2,   // a door, or a crusher coming down
};

struct SectorMotion {
    u32          sector = 0;
    MotionType   type = MotionType::Static;
    MotionTarget target = MotionTarget::Both;

    // The geometry as authored. Motion is expressed as an offset from this
    // rather than accumulated into the live points, so a moving sector cannot
    // drift: after ten thousand ticks it is still exactly base + f(tick), not
    // base plus ten thousand rounded increments.
    fx2 base_points[geom::MAX_POLY_POINTS]{};
    u16 base_point_count = 0;
    fx  base_floor{};
    fx  base_ceil{};

    // Parametric parameters. Replicated once (docs/protocol.md §8.2).
    fx3 from{};
    fx3 to{};
    u32 start_tick = 0;
    u32 duration_ticks = 0;
    LoopMode loop = LoopMode::Once;

    // Current and previous offsets. The difference is the sector's velocity for
    // this tick, which is what an entity inherits on detach (§3.2.5).
    fx3 offset{};
    fx3 prev_offset{};

    // How far the FLOOR moved this tick. This, not the raw offset, is what an
    // entity riding the sector experiences: a descending ceiling displaces
    // nothing an entity is standing on, and carrying riders by the raw offset
    // would drag everyone in a closing door down through the floor with it.
    fx3 floor_delta() const noexcept {
        return fx3{offset.x - prev_offset.x,
                   offset.y - prev_offset.y,
                   target == MotionTarget::Ceiling ? fx::zero() : offset.z - prev_offset.z};
    }

    // Velocity of the surface a rider stands on. Inherited once on detach --
    // (pos_now - pos_last) / dt, exactly as the design specifies.
    fx3 velocity() const noexcept { return floor_delta() * fx::from_int(TICK_RATE_HZ); }
};

// Offset of a parametric motion at `tick`. A pure function of its arguments --
// which is precisely what lets the client evaluate it for a tick the server has
// not reached yet.
fx3 parametric_offset(const SectorMotion& m, u32 tick) noexcept;

class SectorMotionSet {
public:
    SectorMotionSet() noexcept { clear(); }

    void clear() noexcept {
        count_ = 0;
        for (u32 i = 0; i < geom::MAX_MAP_SECTORS; ++i) motion_of_sector_[i] = -1;
    }
    u32 count() const noexcept { return count_; }
    const SectorMotion& at(u32 i) const noexcept { return motions_[i]; }
    SectorMotion& at_mut(u32 i) noexcept { return motions_[i]; }

    // Register a sector as movable, capturing its authored geometry as the
    // base. Returns MAX_MOVING_SECTORS on failure.
    u32 add(const geom::MapStorage& map, u32 sector) noexcept;

    i32 find(u32 sector) const noexcept;

    // Start a parametric motion. Lua-triggered; from here it runs predictably.
    bool start_parametric(u32 sector, fx3 from, fx3 to, u32 start_tick,
                          u32 duration_ticks, LoopMode loop,
                          MotionTarget target = MotionTarget::Both) noexcept;

    // Set an offset directly. Unpredictable by construction, so an anchored
    // entity falls back to anchor-relative prediction on the client.
    bool set_discrete(u32 sector, fx3 offset,
                      MotionTarget target = MotionTarget::Both) noexcept;

    // Advance every motion to `tick` and write the resulting geometry into the
    // map. Parametric sectors are evaluated; discrete ones keep whatever was
    // set. Called once per tick, before agents move.
    //
    // Rail waypoints anchored to a moving sector are rewritten here too, from
    // their authored base plus the same offset the sector's corner points get.
    // That is the whole of §3.4's "sector-relative waypoints": a rail on a
    // moving platform is already in the right place by the time anything reads
    // it, so no rail query, no controller and no renderer has a special case
    // for the combination.
    void apply(geom::MapStorage& map, u32 tick) noexcept;

    void hash_into(StateHasher& h) const noexcept;

private:
    SectorMotion motions_[MAX_MOVING_SECTORS]{};
    u32 count_ = 0;

    // sector -> index into motions_, or -1. Maintained by add(), so the
    // per-tick waypoint pass is one lookup per waypoint instead of a scan of
    // every motion for every waypoint.
    i16 motion_of_sector_[geom::MAX_MAP_SECTORS];
};

}  // namespace dnp::sim
