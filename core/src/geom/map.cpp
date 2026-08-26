#include "dnp/geom/map.hpp"

namespace dnp::geom {

bool point_in_sector(const MapData& map, u32 s, fx2 p) noexcept {
    const Sector& sec = map.sector(s);
    if (sec.point_count < 3) return false;

    bool inside = false;
    const u32 n = sec.point_count;
    for (u32 i = 0, j = n - 1; i < n; j = i++) {
        const fx2& pi = map.point(sec.first_point + i);
        const fx2& pj = map.point(sec.first_point + j);

        // Half-open rule on the vertical span: an edge counts if p.y lies in
        // [min, max) of its endpoints. This is what keeps a point that is
        // exactly level with a shared vertex from being counted twice (or zero
        // times) by the two edges meeting there.
        if ((pi.y > p.y) == (pj.y > p.y)) continue;

        // Cross-product form of "is p.x left of the edge's intersection with
        // the horizontal line through p". Division-free and exact in 128-bit:
        //     (p.x - xi) * (yj - yi)  vs  (xj - xi) * (p.y - yi)
        const i128 dy  = static_cast<i128>(pj.y.raw) - pi.y.raw;
        const i128 lhs = (static_cast<i128>(p.x.raw) - pi.x.raw) * dy;
        const i128 rhs = (static_cast<i128>(pj.x.raw) - pi.x.raw) *
                         (static_cast<i128>(p.y.raw) - pi.y.raw);

        // The comparison flips with the edge's vertical direction, because
        // multiplying by a negative dy reverses the inequality.
        if (dy > 0 ? (lhs < rhs) : (lhs > rhs)) inside = !inside;
    }
    return inside;
}

i32 find_sector(const MapData& map, fx2 p) noexcept {
    // The SMALLEST sector containing the point, not the first one declared.
    //
    // Footprints may nest -- a pillar standing inside a hall, a pit sunk into a
    // floor -- and for a point inside the inner one, both contain it. First
    // match makes the answer depend on declaration order, which shows up as a
    // pillar you can be blocked by but cannot stand on: the mover asks which
    // sector you are in, is told the hall, and drops you to the hall's floor
    // through the pillar under your feet.
    //
    // Smallest-area is the most SPECIFIC answer, and it is a property of the
    // geometry rather than of the file's ordering. Areas are exact 128-bit
    // integers, so the choice is identical on every target -- which it must be,
    // because this predicate decides which floor a body stands on.
    //
    // Partially overlapping footprints remain ill-defined and tools/mapc
    // rejects them; nesting is the case this makes well-defined.
    i32  best = NO_SECTOR;
    i128 best_area = 0;
    for (u32 s = 0; s < map.sector_count; ++s) {
        if (!point_in_sector(map, s, p)) continue;
        if (best == NO_SECTOR) { best = static_cast<i32>(s); continue; }

        // Only computed once a second sector claims the point, so the ordinary
        // case -- exactly one match -- costs nothing beyond the scan it already
        // did.
        if (best_area == 0) {
            best_area = signed_area_2x(map, static_cast<u32>(best));
            if (best_area < 0) best_area = -best_area;
        }
        i128 area = signed_area_2x(map, s);
        if (area < 0) area = -area;
        if (area < best_area) { best = static_cast<i32>(s); best_area = area; }
    }
    return best;
}

fx2 closest_point_on_segment(fx2 a, fx2 b, fx2 p) noexcept {
    const fx2 ab = b - a;
    const fx2 ap = p - a;

    // len_sq at raw scale 2^64; a degenerate segment collapses to its endpoint
    // rather than dividing by zero.
    const u128 len_sq = len_sq_raw(ab);
    if (len_sq == 0) return a;

    // t = dot(ap, ab) / |ab|^2, computed in 128-bit so a long wall does not
    // overflow the numerator before the divide.
    const i128 dot_raw = static_cast<i128>(ap.x.raw) * ab.x.raw +
                         static_cast<i128>(ap.y.raw) * ab.y.raw;

    if (dot_raw <= 0) return a;
    if (static_cast<u128>(dot_raw) >= len_sq) return b;

    // t in Q32.32: (dot << 32) / len_sq. Both sides are at scale 2^64, so the
    // shift restores the fractional scale.
    const i128 t_raw = (dot_raw << fx::FRAC) / static_cast<i128>(len_sq);
    const fx t = fx::from_raw(static_cast<i64>(t_raw));
    return a + ab * t;
}

u128 distance_sq_to_segment(fx2 a, fx2 b, fx2 p) noexcept {
    return len_sq_raw(p - closest_point_on_segment(a, b, p));
}

namespace {
// 2D cross product at raw scale 2^64, kept in 128-bit so a long wall cannot
// overflow the term before it is compared.
inline i128 cross_raw(fx2 v, fx2 w) noexcept {
    return static_cast<i128>(v.x.raw) * w.y.raw - static_cast<i128>(v.y.raw) * w.x.raw;
}
}  // namespace

bool segment_intersect(fx2 p0, fx2 p1, fx2 a, fx2 b, fx& t_out) noexcept {
    const fx2 d1 = p1 - p0;
    const fx2 d2 = b - a;
    const i128 denom = cross_raw(d1, d2);
    if (denom == 0) return false;   // parallel or degenerate; collinear is not a crossing

    const fx2 ap = a - p0;
    const i128 num_t = cross_raw(ap, d2);
    const i128 num_u = cross_raw(ap, d1);

    // Range-check t and u in [0,1] WITHOUT dividing, by comparing against the
    // denominator with its sign accounted for. Dividing first would round, and
    // a rounded boundary is exactly where a crossing gets missed.
    if (denom > 0) {
        if (num_t < 0 || num_t > denom) return false;
        if (num_u < 0 || num_u > denom) return false;
    } else {
        if (num_t > 0 || num_t < denom) return false;
        if (num_u > 0 || num_u < denom) return false;
    }

    t_out = fx::from_raw(static_cast<i64>((num_t << fx::FRAC) / denom));
    return true;
}

fx2 side_normal(const MapData& map, u32 side_index, u32 from_sector) noexcept {
    const Side& sd = map.side(side_index);
    const fx2 a = map.point(sd.a);
    const fx2 b = map.point(sd.b);
    const fx2 edge = b - a;
    if (edge.x.raw == 0 && edge.y.raw == 0) return fx2::zero();

    // Right-hand perpendicular of the edge.
    fx2 n = normalize(fx2{edge.y, -edge.x});

    // Orient it away from the sector. Testing against the polygon's centroid
    // rather than assuming a winding order means a map with inconsistent
    // winding produces a wrong-looking wall, not a player falling through it.
    const Sector& sec = map.sector(from_sector);
    if (sec.point_count >= 3) {
        fx2 centroid = fx2::zero();
        for (u32 i = 0; i < sec.point_count; ++i) centroid += map.point(sec.first_point + i);
        centroid = fx2{centroid.x / static_cast<i64>(sec.point_count),
                       centroid.y / static_cast<i64>(sec.point_count)};
        const fx2 mid = fx2{(a.x + b.x) / 2, (a.y + b.y) / 2};
        if (dot(n, mid - centroid) < fx::zero()) n = -n;
    }
    return n;
}

i128 signed_area_2x(const MapData& map, u32 s) noexcept {
    const Sector& sec = map.sector(s);
    i128 acc = 0;
    const u32 n = sec.point_count;
    for (u32 i = 0, j = n - 1; i < n; j = i++) {
        const fx2& pi = map.point(sec.first_point + i);
        const fx2& pj = map.point(sec.first_point + j);
        // Shoelace, accumulated at raw scale 2^64. Kept in 128-bit rather than
        // reduced to fx per term: a large sector's terms would saturate.
        acc += static_cast<i128>(pj.x.raw) * pi.y.raw -
               static_cast<i128>(pi.x.raw) * pj.y.raw;
    }
    return acc;
}

}  // namespace dnp::geom
