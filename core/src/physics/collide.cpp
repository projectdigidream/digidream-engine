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

bool sector_admits(const MapData& map, u32 s, fx feet_z, fx body_height) noexcept {
    const Sector& to = map.sector(s);

    // The step-up rule, in one place. A rise of at most STEP_HEIGHT is walkable;
    // anything more is a wall. Slopes are not a separate concept -- they are a
    // sequence of small rises, so whether one is walkable falls out of applying
    // this same threshold incrementally.
    if (to.floor_z - feet_z > sim::STEP_HEIGHT) return false;

    // And the space has to be tall enough to stand in. Without this an agent
    // walks into a crawlspace and ends up intersecting its ceiling.
    if (to.ceil_z - to.floor_z < body_height) return false;
    if (to.ceil_z - feet_z < body_height) return false;

    return true;
}

bool side_is_passable(const MapData& map, u32 side_index, u32 from,
                      fx feet_z, fx body_height) noexcept {
    const Side& sd = map.side(side_index);
    if (sd.flags & side_flag::BLOCKING) return false;
    if (sd.neighbor == NO_SECTOR) {
        // An open edge. There is nothing on the far side to admit the body, and
        // nothing it needs: walking off is allowed, and what happens next is
        // gravity's business.
        return (sd.flags & side_flag::LEDGE) != 0;
    }
    (void)from;
    return sector_admits(map, static_cast<u32>(sd.neighbor), feet_z, body_height);
}

bool sector_has_ledge(const MapData& map, i32 s) noexcept {
    if (s < 0 || static_cast<u32>(s) >= map.sector_count) return false;
    const Sector& sec = map.sector(static_cast<u32>(s));
    for (u16 k = 0; k < sec.side_count; ++k) {
        if (map.side(sec.first_side + k).flags & side_flag::LEDGE) return true;
    }
    return false;
}

// Push `p` out of any solid side of `s` it overlaps, and remove the into-wall
// component of `vel` for each contact.
//
// Velocity is handled HERE, not only on the sweep path, because contact and
// crossing are different events and the common one is contact. An agent walking
// into a wall reaches resting distance before its centre ever crosses the side,
// so a version that only slid on crossing would leave the full into-wall
// velocity intact: the agent would be shoved back out every tick, jitter against
// the wall, and never settle.
SweepContact push_out_of_sector(const MapData& map, u32 s, fx2& p, fx3* vel, fx radius,
                                fx feet_z, fx body_height) noexcept {
    const Sector& sec = map.sector(s);
    SweepContact out;
    const fx2 start = p;

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

        out.contacted = true;

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
        if (vel) {
            const fx2 v2 = fx2{vel->x, vel->y};
            if (dot(v2, n) < fx::zero()) {
                const fx2 slid = project_out(v2, n);
                vel->x = slid.x;
                vel->y = slid.y;
            }
        }
    }
    out.escape = p - start;
    return out;
}

SweepContact sweep_out_of_sector(const MapData& map, u32 s, fx2& p, fx radius,
                                 fx feet_z, fx body_height) noexcept {
    SweepContact out;
    const fx2 start = p;
    const Sector& sec = map.sector(s);

    // A moving sector is a solid VOLUME to this agent exactly when the agent
    // could not stand in it. That is the whole rule, and it is deliberately not
    // the per-side passability test push_out_of_sector uses: passability asks
    // "may I leave through this wall", which is the wrong question once the
    // wall has swept past and the agent is standing inside the polygon. Asked
    // the wrong way round, every side reports the agent's own room as a
    // perfectly good destination and the piston slides through it untouched.
    const bool volume_solid = !sector_admits(map, s, feet_z, body_height);

    const u128 r_sq = static_cast<u128>(static_cast<i128>(radius.raw) * radius.raw);

    const auto side_is_solid = [&](u32 si) {
        const Side& sd = map.side(si);
        return volume_solid || (sd.flags & side_flag::BLOCKING) != 0 || sd.neighbor == NO_SECTOR;
    };

    // Phase 1: the footprint has swept OVER the agent, which is the case a
    // resting-contact test cannot see -- the agent is not near one wall, it is
    // inside all of them. Eject through the nearest boundary only. Pushing out
    // of every overlapped side would add four normals together and fire the
    // agent out of a corner at several times the speed the sector was moving.
    if (volume_solid && point_in_sector(map, s, p)) {
        i32  nearest = -1;
        u128 best_d = 0;
        for (u32 k = 0; k < sec.side_count; ++k) {
            const u32 si = sec.first_side + k;
            const Side& sd = map.side(si);
            const u128 d = distance_sq_to_segment(map.point(sd.a), map.point(sd.b), p);
            // Strictly nearer, so ties resolve to the lower index rather than to
            // whichever the loop happened to reach last.
            if (nearest < 0 || d < best_d) { best_d = d; nearest = static_cast<i32>(si); }
        }
        if (nearest >= 0) {
            const fx2 n = side_normal(map, static_cast<u32>(nearest), s);   // outward
            if (n.x.raw != 0 || n.y.raw != 0) {
                const fx dist = fx::from_raw(static_cast<i64>(isqrt128(best_d)));
                p += n * (radius + dist);
                out.contacted = true;
            }
        }
    }

    // Phase 2: ordinary resting overlap against the solid sides, from outside.
    for (u32 k = 0; k < sec.side_count; ++k) {
        const u32 si = sec.first_side + k;
        if (!side_is_solid(si)) continue;

        const Side& sd = map.side(si);
        const fx2 a = map.point(sd.a);
        const fx2 b = map.point(sd.b);

        const fx2 closest = closest_point_on_segment(a, b, p);
        const fx2 away = p - closest;
        const u128 d_sq = len_sq_raw(away);
        if (d_sq >= r_sq) continue;

        out.contacted = true;

        // Same normal choice as resting contact: the side's own normal against
        // its interior (exact for axis-aligned geometry), radial at an endpoint
        // so an agent grazing a corner is not ejected along the wall.
        const bool at_endpoint = (closest == a) || (closest == b);
        fx2 n;
        if (at_endpoint && d_sq != 0) {
            n = normalize(away);
        } else {
            n = side_normal(map, si, s);
            if (dot(n, away) < fx::zero()) n = -n;
            if (n.x.raw == 0 && n.y.raw == 0) n = (d_sq == 0) ? fx2::zero() : normalize(away);
        }
        if (n.x.raw == 0 && n.y.raw == 0) continue;

        const fx dist = fx::from_raw(static_cast<i64>(isqrt128(d_sq)));
        p += n * (radius - dist);
    }

    out.escape = p - start;
    return out;
}

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

    // May this entity end the tick outside the map?
    //
    // Only from a sector that has an open edge, or if it is already out there
    // falling. The distinction matters both ways: refusing unconditionally --
    // which is what this did before floating platforms existed -- makes every
    // platform edge an invisible wall, and allowing unconditionally turns a
    // geometry error into an entity falling forever with nothing to say why.
    const SurfaceQuery q_start = query_surface(map, pos2, r.sector);
    bool may_leave = in.sector == NO_SECTOR || !q_start.in_map ||
                     sector_has_ledge(map, q_start.sector);

    for (u32 iter = 0; iter < MAX_SLIDE_ITERATIONS; ++iter) {
        if (remaining.x.raw == 0 && remaining.y.raw == 0) break;
        r.iterations = iter + 1;

        const SurfaceQuery q0 = query_surface(map, pos2, r.sector);
        if (!q0.in_map) {
            // Out in the void: no geometry, so the whole remaining displacement
            // applies. Falling entities keep the horizontal speed they left
            // with, which is what makes a missed jump look like a missed jump
            // rather than like hitting something.
            pos2 += remaining;
            break;
        }
        r.sector = q0.sector;
        // Re-checked per iteration rather than only at the start: a slide can
        // carry an entity into a neighbouring sector and out through ITS ledge
        // within one tick.
        if (!may_leave) may_leave = sector_has_ledge(map, q0.sector);

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
            if (push_out_of_sector(map, static_cast<u32>(q.sector), pos2, &r.vel,
                                   in.radius, r.pos.z, in.height).contacted) {
                r.hit_wall = true;
            }
        } else if (may_leave) {
            // Off the edge. No sector, no floor, no contact -- the vertical
            // pass below finds no ground and gravity does the rest.
            r.sector = NO_SECTOR;
            r.grounded = false;
        } else {
            // Outside the map with no way it should have got there. Refuse:
            // staying put is a visible bug, and falling out of a closed room
            // forever is an invisible one.
            pos2 = fx2{r.pos.x, r.pos.y};
            r.hit_wall = true;
        }
    }

    r.pos.x = pos2.x;
    r.pos.y = pos2.y;

    // --- settle the sector, then vertical ------------------------------------

    const SurfaceQuery q = query_surface(map, fx2{r.pos.x, r.pos.y}, r.sector);

    // Passing UNDER a platform.
    //
    // A sector is a 2D footprint, so an entity falling in the void that drifts
    // sideways beneath one is "inside" it as far as the map is concerned -- and
    // the ground rule below would snap it up onto that floor. It would be
    // teleported, from underneath, onto the platform it had just missed.
    //
    // So an entity that was ALREADY outside the map does not land on a floor it
    // is below; it keeps falling past it. Scoped to entities already in the
    // void, which is a state that could not exist before open edges did, so no
    // closed map's behaviour changes by a single bit. In particular a floor
    // rising into someone standing on it still lifts them: they are in a
    // sector, not in the void.
    const bool under_platform =
        q.in_map && in.sector == NO_SECTOR && r.pos.z < q.floor_z;
    const bool in_world = q.in_map && !under_platform;

    r.sector = in_world ? q.sector : NO_SECTOR;

    const fx floor_z = in_world ? q.floor_z : r.pos.z;
    const fx ceil_z = in_world ? q.ceil_z : (r.pos.z + in.height);

    // Step up: the horizontal move landed on ground above the feet, but within
    // the threshold, so lift onto it and keep the motion.
    if (in_world && floor_z > r.pos.z && floor_z - r.pos.z <= sim::STEP_HEIGHT) {
        r.pos.z = floor_z;
        r.stepped_up = true;
        if (r.vel.z < fx::zero()) r.vel.z = fx::zero();
        r.grounded = true;
    }

    r.pos.z += r.vel.z * dt;

    if (in_world && r.pos.z <= floor_z) {
        r.pos.z = floor_z;
        if (r.vel.z < fx::zero()) r.vel.z = fx::zero();
        r.grounded = true;
    } else if (in_world && in.grounded && !r.stepped_up &&
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

    if (in_world && r.pos.z + in.height > ceil_z) {
        r.pos.z = ceil_z - in.height;
        if (r.vel.z > fx::zero()) r.vel.z = fx::zero();
        r.hit_ceiling = true;
        // A ceiling that leaves less headroom than the body height means the
        // agent is being compressed. The clamp keeps the body between the two
        // surfaces so nothing downstream sees an agent below its own floor;
        // recognising it AS a crush is sim/crush.cpp's job, because the answer
        // depends on the sector, not on this one agent's move.
        if (r.pos.z < floor_z) r.pos.z = floor_z;
    }

    return r;
}

i32 find_side_with_flag(const MapData& map, i32 sector, fx2 p, fx radius,
                        u8 flag, fx2& out_normal) noexcept {
    if (sector < 0 || static_cast<u32>(sector) >= map.sector_count) return -1;

    const Sector& sec = map.sector(static_cast<u32>(sector));
    const u128 r_sq = static_cast<u128>(static_cast<i128>(radius.raw) * radius.raw);

    i32  best = -1;
    u128 best_d = r_sq;
    for (u32 k = 0; k < sec.side_count; ++k) {
        const u32 si = sec.first_side + k;
        if ((map.side(si).flags & flag) == 0) continue;

        const Side& sd = map.side(si);
        const u128 d_sq = distance_sq_to_segment(map.point(sd.a), map.point(sd.b), p);
        // Strictly nearer, so ties resolve to the lower index rather than to
        // whichever the loop happened to reach last.
        if (d_sq < best_d) {
            best_d = d_sq;
            best = static_cast<i32>(si);
        }
    }

    if (best < 0) return -1;
    // Inward: from the wall toward the agent. See the header.
    out_normal = -side_normal(map, static_cast<u32>(best), static_cast<u32>(sector));
    return best;
}

RayHit raycast(const MapData& map, i32 sector, fx2 from, fx2 dir, fx max_dist,
               u8 flag) noexcept {
    RayHit hit;
    if (sector < 0 || static_cast<u32>(sector) >= map.sector_count) return hit;

    const fx2 to = from + dir * max_dist;
    i32 cur = sector;

    // Walk sector to sector. Bounded so a ray that grazes a shared edge and
    // oscillates between two sectors costs a fixed amount rather than hanging
    // the tick.
    for (int hops = 0; hops < 32; ++hops) {
        const Sector& sec = map.sector(static_cast<u32>(cur));

        fx   best_t = fx::one();
        i32  best_side = -1;
        bool found = false;

        for (u32 k = 0; k < sec.side_count; ++k) {
            const u32 si = sec.first_side + k;
            const Side& sd = map.side(si);
            fx t;
            if (!segment_intersect(from, to, map.point(sd.a), map.point(sd.b), t)) continue;
            // Ignore a crossing at the very start, which is the side just
            // stepped through on the previous hop.
            if (t <= fx::from_frac(1, 4096)) continue;
            if (!found || t < best_t) { best_t = t; best_side = static_cast<i32>(si); found = true; }
        }

        if (!found) return hit;   // ran out of range inside this sector

        const Side& sd = map.side(static_cast<u32>(best_side));
        const bool wanted = (sd.flags & flag) != 0;
        const bool solid = (sd.neighbor == NO_SECTOR) || (sd.flags & side_flag::BLOCKING) != 0;

        if (wanted) {
            hit.hit = true;
            hit.side = best_side;
            hit.point = from + (to - from) * best_t;
            hit.normal = side_normal(map, static_cast<u32>(best_side), static_cast<u32>(cur));
            hit.distance = max_dist * best_t;
            return hit;
        }
        // A solid side without the flag occludes: you cannot grapple through a
        // wall just because the anchor beyond it is in range.
        if (solid) return hit;

        cur = sd.neighbor;
    }
    return hit;
}

}  // namespace dnp::physics
