#include "dnp/physics/collide.hpp"

namespace dnp::physics {

using namespace dnp::geom;

namespace {

// Slide iterations per tick. Three is enough to resolve a corner (wall, then
// the wall it slid into, then a final check); an unbounded loop would let
// pathological geometry cost unbounded time on the server.
constexpr u32 MAX_SLIDE_ITERATIONS = 3;

}  // namespace

SurfaceQuery query_surface(const MapData& map, fx2 p, i32 hint) noexcept {
    SurfaceQuery q;

    // Try the hint first. An agent usually stays in the sector it was in, so
    // this turns the common case into one polygon test instead of a scan.
    if (hint >= 0 && static_cast<u32>(hint) < map.sector_count &&
        point_in_sector(map, static_cast<u32>(hint), p)) {
        q.sector = hint;
    } else {
        q.sector = find_sector(map, p);
    }

    if (q.sector == NO_SECTOR) return q;
    q.in_map = true;
    q.floor_z = map.sector(static_cast<u32>(q.sector)).floor_z;
    q.ceil_z = map.sector(static_cast<u32>(q.sector)).ceil_z;
    return q;
}

bool side_is_passable(const MapData& map, u32 side_index, u32 from,
                      fx feet_z, fx body_height) noexcept {
    const Side& sd = map.side(side_index);
    if (sd.flags & side_flag::BLOCKING) return false;
    if (sd.neighbor == NO_SECTOR) return false;

    const Sector& to = map.sector(static_cast<u32>(sd.neighbor));
    (void)from;

    // The step-up rule, in one place. A rise of at most STEP_HEIGHT is walkable;
    // anything more is a wall. Slopes are not a separate concept -- they are a
    // sequence of small rises, so whether one is walkable falls out of applying
    // this same threshold incrementally.
    if (to.floor_z - feet_z > sim::STEP_HEIGHT) return false;

    // And the far side has to be tall enough to stand in. Without this an agent
    // walks into a crawlspace and ends up intersecting its ceiling.
    if (to.ceil_z - to.floor_z < body_height) return false;
    if (to.ceil_z - feet_z < body_height) return false;

    return true;
}

namespace {

// Push `p` out of any blocking side it overlaps, and remove the into-wall
// component of `vel` for each contact. Returns true if anything was touched.
//
// Velocity is handled HERE, not only on the sweep path, because contact and
// crossing are different events and the common one is contact. An agent walking
// into a wall reaches resting distance before its centre ever crosses the side,
// so a version that only slid on crossing would leave the full into-wall
// velocity intact: the agent would be shoved back out every tick, jitter against
// the wall, and never settle.
bool resolve_walls_in_sector(const MapData& map, u32 s, fx2& p, fx3& vel, fx radius,
                             fx feet_z, fx body_height) noexcept {
    const Sector& sec = map.sector(s);
    bool touched = false;

    const u128 r_sq = static_cast<u128>(static_cast<i128>(radius.raw) * radius.raw);

    for (u32 k = 0; k < sec.side_count; ++k) {
        const u32 si = sec.first_side + k;
        if (side_is_passable(map, si, s, feet_z, body_height)) continue;

        const Side& sd = map.side(si);
        const fx2 a = map.point(sd.a);
        const fx2 b = map.point(sd.b);

        const fx2 closest = closest_point_on_segment(a, b, p);
        const fx2 away = p - closest;
        const u128 d_sq = len_sq_raw(away);
        if (d_sq >= r_sq) continue;

        touched = true;

        // Choosing the push direction.
        //
        // Against a wall's INTERIOR the correct direction is the wall's own
        // normal, and using it matters for more than tidiness: deriving the
        // normal from `p - closest` instead carries every bit of accumulated
        // rounding in the position, so an axis-aligned wall yields a normal a
        // hair off-axis and the projection leaks a little velocity between the
        // two axes on every tick of contact. Measured at ~4e-8 per tick, which
        // is invisible per tick and is exactly the kind of thing that turns into
        // a slow drift along a wall. The side's own normal is exact for
        // axis-aligned geometry, which level geometry mostly is.
        //
        // At an ENDPOINT the radial direction is right instead -- that is what
        // lets an agent round a corner rather than being ejected sideways along
        // a wall it is only touching the tip of.
        const bool at_endpoint = (closest == a) || (closest == b);
        fx2 n;
        if (at_endpoint && d_sq != 0) {
            n = normalize(away);
        } else {
            n = side_normal(map, si, s);
            // Orient it from the wall toward the agent.
            if (dot(n, away) < fx::zero()) n = -n;
            if (n.x.raw == 0 && n.y.raw == 0) n = (d_sq == 0) ? fx2::zero() : normalize(away);
        }
        if (n.x.raw == 0 && n.y.raw == 0) continue;

        const fx dist = fx::from_raw(static_cast<i64>(isqrt128(d_sq)));
        p += n * (radius - dist);

        // Wall sliding on contact: drop the component heading into this wall,
        // keep the tangential part. Applied per contact, so a corner resolves
        // against both walls rather than only the last one examined.
        const fx2 v2 = fx2{vel.x, vel.y};
        if (dot(v2, n) < fx::zero()) {
            const fx2 slid = project_out(v2, n);
            vel.x = slid.x;
            vel.y = slid.y;
        }
    }
    return touched;
}

}  // namespace

MoveResult move(const MapData& map, const MoveInput& in, fx dt) noexcept {
    MoveResult r;
    r.pos = in.pos;
    r.vel = in.vel;
    r.sector = in.sector;

    const fx2 delta = fx2{in.vel.x * dt, in.vel.y * dt};

    // --- horizontal: swept crossing test, then slide, then push out ----------
    //
    // Three distinct jobs, and conflating them is where this normally goes
    // wrong:
    //   1. the SWEEP guarantees the centre never crosses a blocking side,
    //      whatever the speed -- this is CCD, and it is a path test, not a
    //      finer sampling rate;
    //   2. the SLIDE keeps tangential velocity so corners are not sticky;
    //   3. the PUSH-OUT resolves resting overlap, which the sweep alone cannot
    //      because contact is a distance, not a crossing.

    fx2 pos2 = fx2{r.pos.x, r.pos.y};
    fx2 remaining = delta;

    for (u32 iter = 0; iter < MAX_SLIDE_ITERATIONS; ++iter) {
        if (remaining.x.raw == 0 && remaining.y.raw == 0) break;
        r.iterations = iter + 1;

        const SurfaceQuery q0 = query_surface(map, pos2, r.sector);
        if (!q0.in_map) break;
        r.sector = q0.sector;

        const fx2 target = pos2 + remaining;

        // Earliest blocking crossing along the whole path this iteration.
        fx  best_t = fx::one();
        i32 best_side = -1;
        bool crossed = false;

        const Sector& sec = map.sector(static_cast<u32>(q0.sector));
        for (u32 k = 0; k < sec.side_count; ++k) {
            const u32 si = sec.first_side + k;
            if (side_is_passable(map, si, static_cast<u32>(q0.sector), r.pos.z, in.height)) continue;

            const Side& sd = map.side(si);
            fx t;
            if (!segment_intersect(pos2, target, map.point(sd.a), map.point(sd.b), t)) continue;
            if (!crossed || t < best_t) { best_t = t; best_side = static_cast<i32>(si); crossed = true; }
        }

        if (!crossed) {
            pos2 = target;
            break;
        }

        r.swept = true;
        r.hit_wall = true;

        const fx2 n = side_normal(map, static_cast<u32>(best_side), static_cast<u32>(q0.sector));

        // Stop just short of the crossing. Landing exactly ON the wall would
        // leave the point-in-sector test to decide which side it is on, and
        // that is a coin flip at the boundary.
        const fx stop_t = fx_max(fx::zero(), best_t - fx::from_frac(1, 1024));
        pos2 = pos2 + remaining * stop_t;

        // Slide: drop the into-wall component of both the remaining motion and
        // the velocity, keep the tangential part, and continue with what is
        // left of the tick.
        const fx2 leftover = remaining * (fx::one() - stop_t);
        remaining = project_out(leftover, n);

        const fx2 v2 = fx2{r.vel.x, r.vel.y};
        if (dot(v2, n) < fx::zero()) {
            const fx2 slid = project_out(v2, n);
            r.vel.x = slid.x;
            r.vel.y = slid.y;
        }
    }

    // Push out of any wall the agent is resting against. The sweep prevents
    // crossing; this prevents overlap, which is a separate condition.
    {
        const SurfaceQuery q = query_surface(map, pos2, r.sector);
        if (q.in_map) {
            r.sector = q.sector;
            if (resolve_walls_in_sector(map, static_cast<u32>(q.sector), pos2, r.vel,
                                        in.radius, r.pos.z, in.height)) {
                r.hit_wall = true;
            }
        } else {
            // Outside the map: refuse the move entirely. Falling out of the
            // world is not a recoverable state, so the safe direction is to
            // stay put.
            pos2 = fx2{r.pos.x, r.pos.y};
            r.hit_wall = true;
        }
    }

    r.pos.x = pos2.x;
    r.pos.y = pos2.y;

    // --- settle the sector, then vertical ------------------------------------

    const SurfaceQuery q = query_surface(map, fx2{r.pos.x, r.pos.y}, r.sector);
    if (q.in_map) r.sector = q.sector;

    const fx floor_z = q.in_map ? q.floor_z : r.pos.z;
    const fx ceil_z = q.in_map ? q.ceil_z : (r.pos.z + in.height);

    // Step up: the horizontal move landed on ground above the feet, but within
    // the threshold, so lift onto it and keep the motion.
    if (q.in_map && floor_z > r.pos.z && floor_z - r.pos.z <= sim::STEP_HEIGHT) {
        r.pos.z = floor_z;
        r.stepped_up = true;
        if (r.vel.z < fx::zero()) r.vel.z = fx::zero();
        r.grounded = true;
    }

    r.pos.z += r.vel.z * dt;

    if (q.in_map && r.pos.z <= floor_z) {
        r.pos.z = floor_z;
        if (r.vel.z < fx::zero()) r.vel.z = fx::zero();
        r.grounded = true;
    } else if (q.in_map && in.grounded && !r.stepped_up &&
               r.pos.z - floor_z <= sim::STEP_HEIGHT && r.vel.z <= fx::zero()) {
        // Step down (ground snapping). A grounded agent crossing onto a floor
        // that drops by no more than the step threshold is snapped to it and
        // stays grounded -- no airborne frames. Without this, running down
        // stairs means leaving the ground for a few frames per step, which
        // reads as floaty and also breaks anything gated on `grounded`.
        //
        // The SAME constant governs both directions, so "walkable" is symmetric
        // by construction and there is no second threshold to keep in sync.
        r.pos.z = floor_z;
        r.vel.z = fx::zero();
        r.grounded = true;
        r.stepped_down = true;
    } else {
        r.grounded = false;
    }

    if (q.in_map && r.pos.z + in.height > ceil_z) {
        r.pos.z = ceil_z - in.height;
        if (r.vel.z > fx::zero()) r.vel.z = fx::zero();
        r.hit_ceiling = true;
        // A ceiling that leaves less headroom than the body height means the
        // agent is being compressed. Detection is native; what happens as a
        // result is Lua's decision (design §3.1 crushing), so nothing is
        // decided here beyond reporting the contact.
        if (r.pos.z < floor_z) r.pos.z = floor_z;
    }

    return r;
}

}  // namespace dnp::physics
