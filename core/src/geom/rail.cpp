#include "dnp/geom/rail.hpp"

namespace dnp::geom {

namespace {

// The waypoint at `i` within a rail, wrapping for a loop closure.
const RailWaypoint& wp(const MapData& map, const Rail& r, u32 i) noexcept {
    return map.waypoint(r.first_waypoint + (i % r.waypoint_count));
}

}  // namespace

u32 rail_segment_count(const MapData& map, u32 rail) noexcept {
    if (rail >= map.rail_count) return 0;
    const Rail& r = map.rail(rail);
    if (r.waypoint_count < 2) return 0;
    return (r.flags & rail_flag::LOOP) ? r.waypoint_count : r.waypoint_count - 1u;
}

fx rail_length(const MapData& map, u32 rail) noexcept {
    const u32 segs = rail_segment_count(map, rail);
    if (segs == 0) return fx::zero();
    const Rail& r = map.rail(rail);
    fx total = fx::zero();
    for (u32 i = 0; i < segs; ++i) {
        total += distance(wp(map, r, i).pos, wp(map, r, i + 1).pos);
    }
    return total;
}

RailPoint rail_point_at(const MapData& map, u32 rail, fx along) noexcept {
    RailPoint out;
    if (rail >= map.rail_count) return out;
    const Rail& r = map.rail(rail);
    if (r.waypoint_count == 0) return out;

    out.pos = map.waypoint(r.first_waypoint).pos;
    const u32 segs = rail_segment_count(map, rail);
    if (segs == 0) return out;

    const fx total = rail_length(map, rail);
    if (total.raw <= 0) return out;

    fx d = along;
    if (r.flags & rail_flag::LOOP) {
        // Wrap into [0, total). Written as a loop rather than a modulo because
        // fx has no integer remainder and a divide-and-multiply round-trip
        // would not land exactly on the wrap boundary -- which is the one place
        // a rider must not jitter.
        while (d.raw < 0) d += total;
        while (!(d < total)) d -= total;
    } else {
        if (d.raw < 0) { d = fx::zero(); out.clamped = true; }
        if (d > total) { d = total; out.clamped = true; }
    }

    for (u32 i = 0; i < segs; ++i) {
        const fx3 a = wp(map, r, i).pos;
        const fx3 b = wp(map, r, i + 1).pos;
        const fx seg = distance(a, b);
        out.segment = i;
        if (d <= seg || i + 1 == segs) {
            // Exact at both ends: a rider stopped at a waypoint is AT it, not
            // one ulp short of it, so the next segment's arithmetic starts from
            // the authored position rather than from an accumulated error.
            out.t = seg.raw == 0 ? fx::zero() : fx::from_frac(d.raw, seg.raw);
            out.pos = fx3{fx_lerp(a.x, b.x, out.t),
                          fx_lerp(a.y, b.y, out.t),
                          fx_lerp(a.z, b.z, out.t)};
            return out;
        }
        d -= seg;
    }
    return out;
}

RailNearest rail_nearest(const MapData& map, u32 rail, fx3 p) noexcept {
    RailNearest out;
    if (rail >= map.rail_count) return out;
    const Rail& r = map.rail(rail);
    if (r.waypoint_count == 0) return out;

    out.pos = map.waypoint(r.first_waypoint).pos;
    out.dist_sq = len_sq_raw(p - out.pos);

    const u32 segs = rail_segment_count(map, rail);
    fx travelled = fx::zero();
    for (u32 i = 0; i < segs; ++i) {
        const fx3 a = wp(map, r, i).pos;
        const fx3 b = wp(map, r, i + 1).pos;
        const fx3 ab = b - a;
        const fx seg = length(ab);

        fx t = fx::zero();
        if (seg.raw != 0) {
            // Projection parameter, clamped to the segment. Computed from the
            // dot product over the squared length so there is one divide and no
            // normalisation -- normalising first would round the direction and
            // then round the projection onto it.
            const i128 num = static_cast<i128>(dot(p - a, ab).raw);
            const i128 den = static_cast<i128>(length_sq(ab).raw);
            if (den > 0) {
                if (num <= 0) t = fx::zero();
                else if (num >= den) t = fx::one();
                else t = fx::from_frac(static_cast<i64>(num), static_cast<i64>(den));
            }
        }

        const fx3 q{fx_lerp(a.x, b.x, t), fx_lerp(a.y, b.y, t), fx_lerp(a.z, b.z, t)};
        const u128 d_sq = len_sq_raw(p - q);
        // Strictly nearer, so a point equidistant from two segments resolves to
        // the earlier one rather than to whichever the loop reached last.
        if (i == 0 || d_sq < out.dist_sq) {
            out.dist_sq = d_sq;
            out.pos = q;
            out.segment = i;
            out.distance_along = travelled + seg * t;
        }
        travelled += seg;
    }
    return out;
}

u32 rail_link_count_at(const MapData& map, u32 rail, u16 waypoint) noexcept {
    u32 n = 0;
    for (u32 i = 0; i < map.rail_link_count; ++i) {
        const RailLink& l = map.rail_link(i);
        if (l.rail == rail && l.waypoint == waypoint) ++n;
    }
    return n;
}

i32 rail_link_at(const MapData& map, u32 rail, u16 waypoint, u32 nth) noexcept {
    u32 n = 0;
    for (u32 i = 0; i < map.rail_link_count; ++i) {
        const RailLink& l = map.rail_link(i);
        if (l.rail != rail || l.waypoint != waypoint) continue;
        if (n == nth) return static_cast<i32>(i);
        ++n;
    }
    return -1;
}

}  // namespace dnp::geom
