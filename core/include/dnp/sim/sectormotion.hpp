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

    // AND A TURN, about `pivot`, over the same phase.
    //
    // In brad and SIGNED AND UNWRAPPED, which is why these are i64 and not the
    // u32 every other angle in the engine is: a turn is 2^32, so a u32 endpoint
    // cannot say "three times round" -- 0 to 2^32 is 0 to 0 -- and it cannot
    // say which way. The applied angle is this value taken modulo the turn,
    // which is where it becomes an ordinary brad again.
    //
    // A rotating sector is the same idea as a sliding one and a harder version
    // of it: a slider stops being where the map says it is, and a spinner stops
    // being the SHAPE the map says it is, from the point of view of everything
    // that asks which sector a point is in.
    fx2 pivot{};
    i64 spin_from = 0;
    i64 spin_to = 0;

    // Current and previous offsets. The difference is the sector's velocity for
    // this tick, which is what an entity inherits on detach (§3.2.5).
    fx3 offset{};
    fx3 prev_offset{};

    // Current and previous angle, as an ordinary brad. Kept beside the offsets
    // and for the same reason: a rider is carried by the DIFFERENCE, and a
    // difference needs both ends.
    u32 angle = 0;
    u32 prev_angle = 0;

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

    // Did anything turn this tick? One comparison, so the rotation path costs
    // nothing at all for the lifts and doors that are every other mover.
    bool turning() const noexcept { return angle != prev_angle; }

    // WHERE A POINT STANDING ON THIS SECTOR GOES, this tick.
    //
    // floor_delta() is the same displacement for everybody, which is true of a
    // translation and false of a turn: a rider near the pivot travels a
    // centimetre while one at the rim travels a metre. So a rider is carried
    // by asking where ITS OWN point lands.
    //
    // Derived rather than guessed. The sector's placement is
    // `T(q) = R(angle)*(q - pivot) + pivot + offset` applied to the authored
    // points, so a body standing at `p` last tick was standing on the base
    // point `T_prev^-1(p)`, and this tick that base point is at `T(T_prev^-1(p))`.
    // The pivot and the offsets cancel to leave one rotation by the DIFFERENCE
    // of the two angles, which is the whole of it:
    //
    //     new = R(angle - prev_angle) * (p - prev_offset_xy - pivot) + pivot + offset_xy
    //
    // Reduces to floor_delta() exactly when the two angles are equal, which is
    // asserted rather than assumed.
    fx3 carry_delta(fx2 p) const noexcept;
};

// Offset of a parametric motion at `tick`. A pure function of its arguments --
// which is precisely what lets the client evaluate it for a tick the server has
// not reached yet.
fx3 parametric_offset(const SectorMotion& m, u32 tick) noexcept;

// And its angle at `tick`, on the SAME phase, so a sector that both slides and
// turns arrives at both endpoints together. Integer arithmetic throughout:
// `from + (to - from) * t / duration` is exact at both ends, which is what
// fx_lerp's own contract buys for the offset and what a brad gets for free.
u32 parametric_angle(const SectorMotion& m, u32 tick) noexcept;

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
    // `pivot`, `spin_from` and `spin_to` are the turn; all three default to
    // "no turn", so every existing caller is a pure translation and reads
    // exactly as it did.
    bool start_parametric(u32 sector, fx3 from, fx3 to, u32 start_tick,
                          u32 duration_ticks, LoopMode loop,
                          MotionTarget target = MotionTarget::Both,
                          fx2 pivot = fx2{}, i64 spin_from = 0,
                          i64 spin_to = 0) noexcept;

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
