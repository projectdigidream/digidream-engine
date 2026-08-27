#include "dnp/geom/map.hpp"

#include "dnp/sim/constants.hpp"

namespace dnp::geom {

namespace {

// One loop's contribution to the crossing count, toggling `inside`.
void cross_loop(const MapData& map, u32 first, u32 n, fx2 p, bool& inside) noexcept {
    for (u32 i = 0, j = n - 1; i < n; j = i++) {
        const fx2& pi = map.point(first + i);
        const fx2& pj = map.point(first + j);

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
}

}  // namespace

bool point_in_sector(const MapData& map, u32 s, fx2 p) noexcept {
    const Sector& sec = map.sector(s);
    if (sec.point_count < 3) return false;

    bool inside = false;
    cross_loop(map, sec.first_point, sec.point_count, p, inside);
    for (u16 h = 0; h < sec.hole_count; ++h) {
        const PolyLoop& L = map.hole(sec.first_hole + h);
        cross_loop(map, sec.first_point + L.first, L.count, p, inside);
    }
    return inside;
}

namespace {

// Candidate sectors at a point: the grid's cell if it is usable, the whole map
// if it is not. Deliberately a SUPERSET -- the exact point_in_sector test still
// decides, so the grid can only make the search shorter, never different.
struct Candidates {
    const u16* items = nullptr;   // null means "every sector, by index"
    u32 count = 0;
};

Candidates candidates_at(const MapData& map, fx2 p) noexcept {
    const SectorGrid& g = map.grid;
    if (!g.usable || g.cols == 0 || g.rows == 0 || g.cell.raw <= 0) {
        return Candidates{nullptr, map.sector_count};
    }
    // Integer cell coordinates. Both operands are at the same fixed-point
    // scale, so this is exact integer division and needs no fx arithmetic.
    const i64 dx = p.x.raw - g.min.x.raw;
    const i64 dy = p.y.raw - g.min.y.raw;
    if (dx < 0 || dy < 0) return Candidates{nullptr, 0};
    const i64 cx = dx / g.cell.raw;
    const i64 cy = dy / g.cell.raw;
    if (cx >= g.cols || cy >= g.rows) return Candidates{nullptr, 0};

    const u32 cell = static_cast<u32>(cy) * g.cols + static_cast<u32>(cx);
    const u32 from = g.start[cell];
    const u32 to = g.start[cell + 1];
    return Candidates{g.items + from, to - from};
}

inline u32 candidate_at(const Candidates& c, u32 i) noexcept {
    return c.items ? c.items[i] : i;
}

}  // namespace

u32 sectors_near(const MapData& map, fx2 p, fx radius, u16* out, u32 cap) noexcept {
    if (cap == 0) return 0;
    const SectorGrid& g = map.grid;

    // No usable grid means every sector is a candidate, which is correct and
    // slow -- the same fallback find_sector takes, and for the same reason: the
    // grid may only ever make the search SHORTER, never different.
    if (!g.usable || g.cols == 0 || g.rows == 0 || g.cell.raw <= 0) {
        const u32 n = map.sector_count < cap ? map.sector_count : cap;
        for (u32 i = 0; i < n; ++i) out[i] = static_cast<u16>(i);
        return n;
    }

    // The cells the reach disc touches. Clamped rather than rejected: a point
    // outside the grid can still be within reach of a sector inside it.
    const i64 lo_x = (p.x.raw - radius.raw) - g.min.x.raw;
    const i64 lo_y = (p.y.raw - radius.raw) - g.min.y.raw;
    const i64 hi_x = (p.x.raw + radius.raw) - g.min.x.raw;
    const i64 hi_y = (p.y.raw + radius.raw) - g.min.y.raw;
    i64 cx0 = lo_x < 0 ? 0 : lo_x / g.cell.raw;
    i64 cy0 = lo_y < 0 ? 0 : lo_y / g.cell.raw;
    i64 cx1 = hi_x < 0 ? -1 : hi_x / g.cell.raw;
    i64 cy1 = hi_y < 0 ? -1 : hi_y / g.cell.raw;
    if (cx1 >= g.cols) cx1 = g.cols - 1;
    if (cy1 >= g.rows) cy1 = g.rows - 1;
    if (cx1 < cx0 || cy1 < cy0) return 0;

    u32 n = 0;
    for (i64 cy = cy0; cy <= cy1; ++cy) {
        for (i64 cx = cx0; cx <= cx1; ++cx) {
            const u32 cell = static_cast<u32>(cy) * g.cols + static_cast<u32>(cx);
            for (u32 i = g.start[cell]; i < g.start[cell + 1]; ++i) {
                const u16 s = g.items[i];
                // Linear dedupe. A reach disc spans a handful of cells and each
                // holds a handful of sectors, so the set is tiny and a hash
                // would be a second structure to keep deterministic.
                bool seen = false;
                for (u32 k = 0; k < n && !seen; ++k) seen = out[k] == s;
                if (seen) continue;
                if (n >= cap) return n;
                out[n++] = s;
            }
        }
    }
    return n;
}

i32 find_sector(const MapData& map, fx2 p) noexcept {
    // The SMALLEST sector containing the point, not the first one declared.
    //
    // Footprints may nest -- a pillar standing inside a hall -- and for a point
    // inside the inner one, both contain it. First match makes the answer
    // depend on declaration order. Smallest-area is at least a property of the
    // geometry, and exact: areas are 128-bit integers, so the choice is
    // identical on every target.
    //
    // It is still only an approximation of the real question, which is
    // find_sector_at's.
    const Candidates c = candidates_at(map, p);
    i32  best = NO_SECTOR;
    i128 best_area = 0;
    for (u32 i = 0; i < c.count; ++i) {
        const u32 s = candidate_at(c, i);
        if (!point_in_sector(map, s, p)) continue;
        if (best == NO_SECTOR) { best = static_cast<i32>(s); continue; }

        // Only computed once a second sector claims the point, so the ordinary
        // case -- exactly one match -- costs nothing beyond the scan.
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

i32 find_sector_at(const MapData& map, fx2 p, fx feet_z, fx body_height) noexcept {
    (void)body_height;
    const Candidates c = candidates_at(map, p);

    // The highest floor a body at feet_z could be standing on, and -- if there
    // is none, meaning it is beneath everything here -- the lowest floor above
    // it, which is the "fallen under a platform" case the mover then treats as
    // the void.
    const fx reach = feet_z + sim::STEP_HEIGHT;
    i32 standing = NO_SECTOR;
    i32 lowest = NO_SECTOR;
    for (u32 i = 0; i < c.count; ++i) {
        const u32 s = candidate_at(c, i);
        if (!point_in_sector(map, s, p)) continue;
        const fx floor_z = map.sector(s).floor_z;

        if (floor_z <= reach) {
            // Strictly higher, so equal floors resolve to the lower index
            // rather than to whichever the loop reached last.
            if (standing == NO_SECTOR ||
                floor_z > map.sector(static_cast<u32>(standing)).floor_z) {
                standing = static_cast<i32>(s);
            }
        }
        if (lowest == NO_SECTOR ||
            floor_z < map.sector(static_cast<u32>(lowest)).floor_z) {
            lowest = static_cast<i32>(s);
        }
    }
    return standing != NO_SECTOR ? standing : lowest;
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
    // dot_raw is known positive here (the two branches above returned for <= 0),
    // so the shift is defined -- but written as a multiply anyway, so the file
    // holds one rule rather than one rule and an exception to remember.
    constexpr i128 ONE_SHIFTED = static_cast<i128>(1) << fx::FRAC;
    const i128 t_raw = (dot_raw * ONE_SHIFTED) / static_cast<i128>(len_sq);
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

    // MULTIPLIED, not shifted. num_t is signed and routinely negative -- any
    // crossing where the target segment runs the other way across the ray -- and
    // left-shifting a negative value is undefined behaviour in C++17. It
    // happened to produce the right answer on both native targets, which is the
    // worst way for undefined behaviour to behave: the first compiler to
    // disagree would have moved a determinism hash rather than crashed.
    //
    // UBSan found it the moment the interact search started casting rays in
    // every direction instead of only along a wall's outward side.
    constexpr i128 ONE_SHIFTED = static_cast<i128>(1) << fx::FRAC;
    t_out = fx::from_raw(static_cast<i64>((num_t * ONE_SHIFTED) / denom));
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

namespace {
i128 loop_area_2x(const MapData& map, u32 first, u32 n) noexcept {
    i128 acc = 0;
    for (u32 i = 0, j = n - 1; i < n; j = i++) {
        const fx2& pi = map.point(first + i);
        const fx2& pj = map.point(first + j);
        // Shoelace, accumulated at raw scale 2^64. Kept in 128-bit rather than
        // reduced to fx per term: a large sector's terms would saturate.
        acc += static_cast<i128>(pj.x.raw) * pi.y.raw -
               static_cast<i128>(pi.x.raw) * pj.y.raw;
    }
    return acc;
}
}  // namespace

i128 signed_area_2x(const MapData& map, u32 s) noexcept {
    const Sector& sec = map.sector(s);
    // Holes are stored wound opposite the outer loop, so summing subtracts
    // them and the result is the sector's actual area -- which is what the
    // height-free find_sector's "smallest wins" rule needs to be comparing.
    i128 acc = loop_area_2x(map, sec.first_point, sec.point_count);
    for (u16 h = 0; h < sec.hole_count; ++h) {
        const PolyLoop& L = map.hole(sec.first_hole + h);
        acc += loop_area_2x(map, sec.first_point + L.first, L.count);
    }
    return acc;
}

}  // namespace dnp::geom
