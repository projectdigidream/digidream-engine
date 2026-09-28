#include "dnp/physics/collide.hpp"

namespace dnp::physics {

using namespace dnp::geom;

namespace {

// Slide iterations per tick. Three is enough to resolve a corner (wall, then
// the wall it slid into, then a final check); an unbounded loop would let
// pathological geometry cost unbounded time on the server.
constexpr u32 MAX_SLIDE_ITERATIONS = 3;

}  // namespace

SurfaceQuery query_surface(const MapData& map, fx2 p, i32 hint,
                           fx feet_z, fx body_height) noexcept {
    SurfaceQuery q;

    // THE SECTOR A BODY IS IN IS A FUNCTION OF WHERE IT IS, AND OF NOTHING
    // ELSE -- not of where it came from.
    //
    // This used to try `hint`, the sector the body was in last tick, on the
    // theory that a body usually stays put and one polygon test beats a scan.
    // The footprint test alone accepted it, and with stacked sectors both
    // contain the point in plan, so the hint was believed forever. That was
    // reported twice from maps/mylevel.map, whose lava plane is one polygon
    // containing every platform on the level: first as "jumping on the sector
    // on top transfers you to the lower sector", then as "trying to jump back
    // to the start platform teleports me back to lava".
    //
    // Requiring the hint's floor to be within stepping reach and its ceiling
    // above the feet fixed the first report and not the second: the lava's
    // floor is -3 and its ceiling is 55, so a body jumping out of it satisfies
    // both conditions the whole way up, keeps the hint, and is handed the
    // lava's floor while standing over a platform.
    //
    // Any condition that keeps the hint has to be "the hint is what
    // find_sector_at would answer" -- and once it is that, it is not saving
    // the scan, it is duplicating it. So there is no hint. find_sector_at goes
    // through the broadphase grid, so the common case is a bucket of one or
    // two sectors, which is what the hint was worth in the first place.
    //
    // `hint` stays in the signature: every caller passes it, and the parameter
    // is where the next person will look for this reasoning.
    (void)hint;
    q.sector = find_sector_at(map, p, feet_z, body_height);

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

namespace {

// Keep a cylinder out of a sector treated as a solid volume.
//
// Shared by the two callers that need it. They ask the same geometric question
// for different reasons -- a sector that has MOVED into an entity (§3.1.3), and
// the slab beneath a floating platform's floor -- and they decide solidity
// differently, so that stays the caller's answer to give.
SweepContact push_out_volume(const MapData& map, u32 s, fx2& p, fx radius,
                             bool volume_solid) noexcept {
    SweepContact out;
    const fx2 start = p;
    const Sector& sec = map.sector(s);

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

}  // namespace

SweepContact sweep_out_of_sector(const MapData& map, u32 s, fx2& p, fx radius,
                                 fx feet_z, fx body_height) noexcept {
    // A moving sector is a solid VOLUME to this agent exactly when the agent
    // could not stand in it. That is the whole rule, and it is deliberately not
    // the per-side passability test push_out_of_sector uses: passability asks
    // "may I leave through this wall", which is the wrong question once the
    // wall has swept past and the agent is standing inside the polygon. Asked
    // the wrong way round, every side reports the agent's own room as a
    // perfectly good destination and the piston slides through it untouched.
    return push_out_volume(map, s, p, radius, !sector_admits(map, s, feet_z, body_height));
}

SweepContact push_out_of_slab(const MapData& map, u32 s, fx2& p, fx radius) noexcept {
    // Unconditionally solid: the caller has already established that the entity
    // is below this sector's floor, and below a floor there is platform.
    return push_out_volume(map, s, p, radius, true);
}

bool slab_overlaps(const Sector& sec, fx feet_z, fx body_height) noexcept {
    // The slab occupies [floor - thickness, floor] inside the footprint. A body
    // occupies [feet, feet + height]. They overlap on the standard interval
    // test, and a sector with no thickness has no slab at all -- which is every
    // sector in a sealed room, where "below the floor" is not a place.
    if (sec.thickness <= fx::zero()) return false;
    if (feet_z >= sec.floor_z) return false;                       // above it
    if (feet_z + body_height <= sec.floor_z - sec.thickness) return false;  // below it
    return true;
}

SweepContact resolve_slabs(const MapData& map, fx2& p, fx radius, fx feet_z,
                           fx body_height) noexcept {
    SweepContact out;
    const fx2 start = p;

    // EVERY sector whose slab the body overlaps, not just the one its centre
    // happens to be inside -- and whether the body is in a sector at all is
    // beside the point.
    //
    // That distinction is the whole bug this fixes. A cylinder overlaps a
    // platform's edge while its centre is still outside the footprint, so a
    // test that starts from "which sector am I in" reports nothing until the
    // centre has crossed -- by which point the body has sunk a full radius into
    // the edge. Measured at three ticks of visible penetration before anything
    // reacted.
    //
    // This ran for entities in the VOID only until a map had a tower in it. A
    // body standing on a platform is in a sector, so slabs were never consulted
    // for it, and it walked straight through the tower beside it. The scope was
    // wrong rather than the rule: "below the floor" was not a place anyone
    // could be while every map was a sealed box, and a tower makes it one.
    //
    // The vertical overlap test is slab_overlaps: a slab is a finite box, not a
    // half-space, so a body can be entirely beneath one and pass under it.
    //
    // ONE SECTOR AT A TIME, in ascending index order. Order matters because two
    // pushes can disagree, and index order is the one both the client and the
    // server will always agree on.
    const auto apply = [&](u32 s) {
        if (!slab_overlaps(map.sector(s), feet_z, body_height)) return;
        // A slab blocks a body exactly when the body could not STAND in the
        // sector above it -- the same predicate that decides whether a wall is
        // a wall. That settles the step case for nothing: a sector half a unit
        // higher ADMITS you, so its slab does not stand in the way of the step
        // up onto it, while a tower fifteen units higher does not and does.
        if (sector_admits(map, s, feet_z, body_height)) return;
        const SweepContact hit = push_out_of_slab(map, s, p, radius);
        if (hit.contacted) out.contacted = true;
    };

    // NARROWED BY THE SECTOR GRID, which is the other half of the broadphase
    // this function's own comment used to ask for.
    //
    // Scanning every sector for every agent is what this was, and at a thousand
    // bodies on a 144-sector map a profile put ten of every fourteen samples in
    // step_agent right here. The grid over sector footprints already exists --
    // find_sector uses it, and a property test checks it against a linear scan
    // over thousands of points -- so this is a question it can already answer.
    //
    // WHAT MAY BE SKIPPED, exactly: push_out_volume does nothing unless `p` is
    // inside the footprint or within `radius` of one of its sides. So a sector
    // whose bounding box is further than `radius` from `p` is a no-op, and
    // sectors_near returns a superset of the ones that are not.
    //
    // AND `p` MOVES while the loop runs, so the radius that was enough when the
    // query was made can stop being enough -- the same trap as the agent grid,
    // and answered the same way. Query with a margin, check after every push
    // that the body is still inside it, and if it is not, put `p` back and run
    // the full scan. The full scan IS the definition; the grid only ever gets
    // to make it shorter.
    constexpr u32 MAX_NEAR = 64;
    constexpr i64 MARGIN_UNITS = 2;
    const fx margin = fx::from_int(MARGIN_UNITS);

    bool narrowed = false;
    u32 n_near = 0;
    u16 near_s[MAX_NEAR];
    if (map.grid.usable && map.sector_count > MAX_NEAR) {
        n_near = sectors_near(map, p, radius + margin, near_s, MAX_NEAR);
        // sectors_near stops AT the cap and cannot say whether more were
        // waiting, so a full result is treated as no result.
        narrowed = n_near < MAX_NEAR;
    }

    if (narrowed) {
        // Cell order is not index order, and index order is the contract.
        for (u32 k = 1; k < n_near; ++k) {
            const u16 v = near_s[k];
            u32 m = k;
            while (m > 0 && near_s[m - 1] > v) { near_s[m] = near_s[m - 1]; --m; }
            near_s[m] = v;
        }
        bool escaped = false;
        for (u32 k = 0; k < n_near && !escaped; ++k) {
            apply(near_s[k]);
            const fx dx = p.x - start.x;
            const fx dy = p.y - start.y;
            escaped = dx > margin || dx < -margin || dy > margin || dy < -margin;
        }
        if (!escaped) {
            out.escape = p - start;
            return out;
        }
        p = start;
        out = SweepContact{};
    }

    for (u32 s = 0; s < map.sector_count; ++s) apply(s);

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
    const SurfaceQuery q_start = query_surface(map, pos2, r.sector, r.pos.z, in.height);
    bool may_leave = in.sector == NO_SECTOR || !q_start.in_map ||
                     sector_has_ledge(map, q_start.sector);

    for (u32 iter = 0; iter < MAX_SLIDE_ITERATIONS; ++iter) {
        if (remaining.x.raw == 0 && remaining.y.raw == 0) break;
        r.iterations = iter + 1;

        const SurfaceQuery q0 = query_surface(map, pos2, r.sector, r.pos.z, in.height);
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
        // Every body meets every slab it overlaps, resolved BEFORE asking which
        // sector its centre is in -- see resolve_slabs.
        {
            const SweepContact hit =
                resolve_slabs(map, pos2, in.radius, r.pos.z, in.height);
            if (hit.contacted) {
                r.hit_wall = true;
                // Wall slide, so a falling body brushing a platform's edge
                // drops alongside it instead of stopping dead in mid-air.
                const fx2 n = normalize(hit.escape);
                const fx2 v2 = fx2{r.vel.x, r.vel.y};
                if ((n.x.raw != 0 || n.y.raw != 0) && dot(v2, n) < fx::zero()) {
                    const fx2 slid = project_out(v2, n);
                    r.vel.x = slid.x;
                    r.vel.y = slid.y;
                }
            }
        }

        const SurfaceQuery q = query_surface(map, pos2, r.sector, r.pos.z, in.height);
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

    const SurfaceQuery q = query_surface(map, fx2{r.pos.x, r.pos.y}, r.sector, r.pos.z, in.height);

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

    // The UNDERSIDE of a platform.
    //
    // A body in the void beneath a slab must not rise into it. This is the
    // ceiling rule applied to a surface that is not a ceiling: a sector's
    // ceil_z bounds the room above its floor, and says nothing about the solid
    // hanging below it. Without this you can jump straight up through a
    // platform from underneath and land on top of it.
    if (!in_world && q.in_map) {
        const geom::Sector& sec = map.sector(static_cast<u32>(q.sector));
        if (sec.thickness > fx::zero()) {
            const fx underside = sec.floor_z - sec.thickness;
            if (r.pos.z + in.height > underside && r.pos.z < underside) {
                r.pos.z = underside - in.height;
                if (r.vel.z > fx::zero()) r.vel.z = fx::zero();
                r.hit_ceiling = true;
            }
        }
    }

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

namespace {

// One side the reach ray crossed, and how far along.
struct Crossing {
    fx  t{};
    u32 side = 0;
    u32 owner = 0;
};

}  // namespace

i32 find_interact_side(const MapData& map, i32 sector, fx2 p, brad facing,
                       fx reach, fx feet_z, fx body_height) noexcept {
    // Kept in the signature but unused: whether a reach gets through is a
    // question about the GEOMETRY's opening, not about whether this particular
    // body could walk it. Removing the parameters would be the wrong lesson --
    // an eye-height rule may well want them -- but pretending they are consulted
    // would be worse.
    (void)feet_z;
    (void)body_height;
    // A short RAY along the aim, stopping at the first thing that means
    // something or the first thing you could not walk through.
    //
    // This started out as "the nearest side of your own sector that lies in
    // front of you", which on converted DOOM2 levels reached 2 of MAP01's 5
    // walk-up-able switches: a switch's line very often belongs to a sector you
    // are NOT standing in, and in a room with diagonal walls the nearest side
    // in front is the diagonal beside you rather than the door you are looking
    // at.
    //
    // It then became "the first side the ray crosses", which is better and
    // still wrong in one specific way that MAP02 demonstrates: a switch across
    // an open doorway is unreachable, because the doorway's own two-sided
    // boundary is crossed first. Doom passes straight through an opening a body
    // could fit through (P_LineOpening) and stops at the first line carrying a
    // SPECIAL. So:
    //
    //   USABLE side   -> that is a switch; report it
    //   blocked side  -> a wall, a shut door, a gap too small; report it, since
    //                    it is what you are pressing against
    //   otherwise     -> an opening; keep going
    //
    // Which sides are switches is gameplay, so the MAP declares it. A map that
    // declares nothing still works: the ray stops at the wall in front of you,
    // which is the only candidate a hand-authored map ever has.
    const fx2 dir = fx_dir(facing);
    const fx2 to = p + dir * reach;

    u16 near[64];
    const u32 n = geom::sectors_near(map, p, reach, near, 64);

    constexpr u32 MAX_CROSSINGS = 32;
    Crossing hits[MAX_CROSSINGS];
    u32 count = 0;

    for (u32 c = 0; c < n; ++c) {
        const Sector& sec = map.sector(near[c]);
        for (u16 k = 0; k < sec.side_count && count < MAX_CROSSINGS; ++k) {
            const u32 si = sec.first_side + k;
            const Side& sd = map.side(si);
            fx t{};
            if (!geom::segment_intersect(p, to, map.point(sd.a), map.point(sd.b), t)) continue;
            hits[count++] = Crossing{t, si, near[c]};
        }
    }

    // Insertion sort by distance along the ray. At most a couple of dozen
    // entries and usually two or three, so the simplest ordering that is
    // obviously stable beats anything cleverer.
    //
    // Ties are the shared boundary between two sectors, seen from both owners:
    // one physical wall, two side records, the same crossing point. A USABLE
    // face wins, then the face whose sector the presser is standing in, then
    // the lower index. Getting that order wrong reached the wrong half of every
    // two-sided door line, and since an action is registered on one half only,
    // the door did nothing.
    for (u32 i = 1; i < count; ++i) {
        const Crossing key = hits[i];
        const bool key_use = (map.side(key.side).flags & side_flag::USABLE) != 0;
        const bool key_mine = static_cast<i32>(key.owner) == sector;
        u32 j = i;
        while (j > 0) {
            const Crossing& q = hits[j - 1];
            if (q.t < key.t) break;
            if (q.t == key.t) {
                const bool q_use = (map.side(q.side).flags & side_flag::USABLE) != 0;
                if (q_use && !key_use) break;
                if (q_use == key_use) {
                    const bool q_mine = static_cast<i32>(q.owner) == sector;
                    if (q_mine && !key_mine) break;
                    if (q_mine == key_mine && q.side < key.side) break;
                }
            }
            hits[j] = hits[j - 1];
            --j;
        }
        hits[j] = key;
    }

    for (u32 i = 0; i < count; ++i) {
        const u32 si = hits[i].side;
        const Side& sd = map.side(si);
        if ((sd.flags & side_flag::USABLE) != 0) return static_cast<i32>(si);

        // What stops a reach is the absence of an OPENING, not the absence of a
        // walkable one -- Doom's P_LineOpening, and the distinction is the whole
        // bug. MAP02 puts a switch behind a window 0.83 tall against a 1.17
        // body: you cannot walk through it, and you can obviously see and reach
        // through it. Asking `sector_admits` (which is the movement question,
        // step height and headroom included) made that window a wall and
        // reported the window frame instead of the switch.
        if (sd.neighbor < 0) return static_cast<i32>(si);
        if ((sd.flags & side_flag::BLOCKING) != 0) return static_cast<i32>(si);
        const Sector& here = map.sector(hits[i].owner);
        const Sector& there = map.sector(static_cast<u32>(sd.neighbor));
        const fx top = here.ceil_z < there.ceil_z ? here.ceil_z : there.ceil_z;
        const fx bottom = here.floor_z > there.floor_z ? here.floor_z : there.floor_z;
        if (top <= bottom) return static_cast<i32>(si);   // shut: a door, or a solid step
    }
    return -1;
}

// How many sector boundaries one cast may cross. Generous, because a long shot
// across a converted Doom level legitimately crosses a great many, and running
// out is now a refusal rather than a free pass.
static constexpr int MAX_RAY_HOPS = 256;

// Minimum progress along the ray per hop, as a fraction of its length. Small
// enough not to skip a thin sector, large enough to step clear of the boundary
// just crossed.
static constexpr fx RAY_EPSILON = fx::from_frac(1, 4096);

RayHit raycast(const MapData& map, i32 sector, fx2 from, fx2 dir, fx max_dist,
               u8 flag, fx z, fx z_end) noexcept {
    RayHit hit;
    // No sector to start from means the ray cannot be resolved, and an
    // unresolvable ray must FAIL CLOSED.
    //
    // This returned a bare `hit` -- blocked false, which every occlusion caller
    // reads as "nothing in the way". So a monster whose sector was stale or
    // unset could see, and shoot, through the entire map. Fail-open is the
    // wrong default for a visibility test: the cost of wrongly blocking is a
    // monster that does not notice you, and the cost of wrongly allowing is a
    // monster that shoots you through four walls.
    if (sector < 0 || static_cast<u32>(sector) >= map.sector_count) {
        hit.blocked = true;
        return hit;
    }

    const fx2 to = from + dir * max_dist;
    i32 cur = sector;

    // Walk sector to sector. Bounded so a ray that grazes a shared edge and
    // oscillates between two sectors costs a fixed amount rather than hanging
    // the tick.
    //
    // 32 was not enough for a real level. A zombieman's range is 2048 Doom
    // units, which on MAP01 crosses far more than thirty-two sectors -- and the
    // loop used to fall out of the bottom reporting NOT BLOCKED, so exceeding
    // the budget granted a clear line through everything beyond it. That is the
    // same fail-open as an unset sector and it is the one that survived the
    // first fix.
    // How far along the ray the walk has already got. Every hop must make
    // PROGRESS past this.
    //
    // `from` is never advanced -- `t` is always measured from the original
    // origin -- so the "ignore a crossing at the very start" epsilon below only
    // ever worked on the first hop. On every hop after it, the boundary just
    // stepped through still sits at its own t, was found again as the nearest
    // crossing, and the walk bounced between two sectors until it ran out of
    // hops. Which the old code then reported as NOT BLOCKED, so a ray that
    // failed to resolve granted a clear line through the rest of the map.
    //
    // That is the shoot-through-walls report. The height test added earlier was
    // real but secondary: most rays never got far enough for it to matter.
    fx walked = fx::zero();

    for (int hops = 0; hops < MAX_RAY_HOPS; ++hops) {
        const Sector& sec = map.sector(static_cast<u32>(cur));

        fx   best_t = fx::one();
        i32  best_side = -1;
        bool found = false;

        for (u32 k = 0; k < sec.side_count; ++k) {
            const u32 si = sec.first_side + k;
            const Side& sd = map.side(si);
            fx t;
            if (!segment_intersect(from, to, map.point(sd.a), map.point(sd.b), t)) continue;
            // Strictly beyond where the walk has already reached, so the side
            // just crossed cannot be crossed again.
            if (t <= walked + RAY_EPSILON) continue;
            if (!found || t < best_t) { best_t = t; best_side = static_cast<i32>(si); found = true; }
        }

        if (!found) return hit;   // ran out of range inside this sector

        const Side& sd = map.side(static_cast<u32>(best_side));
        const bool wanted = (sd.flags & flag) != 0;
        bool solid = (sd.neighbor == NO_SECTOR) || (sd.flags & side_flag::BLOCKING) != 0;

        // And, when the caller said what height the ray is at: is the sector
        // beyond this boundary actually OPEN there?
        //
        // A two-sided line is not a hole. Behind one there may be a step taller
        // than the shooter, a balcony floor above its eyeline, or a door stored
        // the way Doom stores a shut door -- a sector whose ceiling rests on its
        // floor, which is a two-sided line with nothing passable behind it at
        // any height at all. Treating every neighbour as transparent is what let
        // a monster see and shoot a player on the far side of several walls.
        if (!solid && z.raw != RAY_NO_HEIGHT.raw) {
            // THE HEIGHT THE RAY IS AT WHERE IT CROSSES, not where it started.
            //
            // With z_end given the ray slopes, so a boundary halfway along is
            // tested at halfway between the two heights. Without it this is `z`
            // and the cast is flat, which is what every caller that does not
            // care about the far end still gets.
            //
            // COMPUTED HERE, not once per hop above. `(z_end - z) * best_t` is
            // an fx multiply, which is a 128-bit multiply, and a hop that ends
            // at a one-sided wall or a BLOCKING side never needs it. Doing it
            // unconditionally cost enough to push the monster script's stall
            // count from 0-1 to 1-2 on maps/monsters -- small, and measured,
            // and there is no reason to pay it.
            const fx z_here = (z_end.raw == RAY_NO_HEIGHT.raw)
                                  ? z
                                  : z + (z_end - z) * best_t;

            // The neighbour's own floor and ceiling, read directly.
            //
            // The first version asked query_surface where a point just past the
            // boundary was, which is two mistakes at once. It is a point-in-
            // polygon search for a sector whose index the side already names --
            // so it scanned the map to rediscover something known, and that
            // search is most of what made the monster script cost four
            // milliseconds a tick. And it is ambiguous: the crossing point lies
            // exactly on an edge belonging to both sectors, so it could answer
            // with the one already left, which reported every wall as open.
            const Sector& beyond = map.sector(static_cast<u32>(sd.neighbor));

            // No gap at all is opaque at every height. That is how Doom stores
            // a shut door -- a sector whose ceiling rests on its floor -- and it
            // needs its own test, because `floor <= z <= ceil` is satisfied by a
            // zero gap at exactly that height.
            if (beyond.ceil_z <= beyond.floor_z) {
                solid = true;                    // opaque at every height
            } else if (z_here < beyond.floor_z) {
                solid = true;
                hit.height_block = -1;           // under the floor beyond
            } else if (z_here > beyond.ceil_z) {
                solid = true;
                hit.height_block = 1;            // over the ceiling beyond
            }
        }

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
        //
        // Reported, not merely acted on. Returning a bare "no" here made every
        // caller that wanted to know WHY unable to find out, and each of them
        // -- sight, bullets, projectiles -- read it as a clear line.
        if (solid) {
            hit.blocked = true;
            hit.side = best_side;
            hit.point = from + (to - from) * best_t;
            hit.normal = side_normal(map, static_cast<u32>(best_side), static_cast<u32>(cur));
            hit.distance = max_dist * best_t;
            return hit;
        }

        cur = sd.neighbor;
        walked = best_t;
    }

    // Out of hops. The ray is unresolved past this point, so it is blocked --
    // for the same reason an unset start sector is.
    hit.blocked = true;
    return hit;
}

// The middle of a column, and then one end of it. See the header.
bool can_see(const geom::MapData& map, i32 from_sector, fx3 eye,
             fx2 target, fx low, fx high) noexcept {
    const fx2 d{target.x - eye.x, target.y - eye.y};
    const fx dist = length(d);
    // Standing inside each other. Not a degenerate case to reject: two bodies
    // at the same plan position see each other, and normalising a zero vector
    // would make that a division instead of an answer.
    if (dist <= fx::zero()) return true;
    const fx2 dir = normalize(d);
    const fx2 from{eye.x, eye.y};

    // BLOCKED BEFORE REACHING IT is a wall in the way; blocked at or past it is
    // the wall behind it.
    //
    // The middle first, because it is the line that succeeds nearly always and
    // the only one anybody pays for.
    const fx mid = low + (high - low) / fx::from_int(2);
    const RayHit first = raycast(map, from_sector, from, dir, dist, 0, eye.z, mid);
    if (!first.blocked || first.distance >= dist) return true;
    // A WALL is a wall from every angle. Retrying against one is the cost this
    // whole arrangement exists to avoid paying on a level full of monsters that
    // genuinely cannot see anybody.
    if (first.height_block == 0) return false;

    // And once more at the end of the column the failure points towards: under
    // a floor, aim at the head; over a ceiling, aim at the feet.
    const RayHit again = raycast(map, from_sector, from, dir, dist, 0, eye.z,
                                 first.height_block < 0 ? high : low);
    return !again.blocked || again.distance >= dist;
}

}  // namespace dnp::physics
