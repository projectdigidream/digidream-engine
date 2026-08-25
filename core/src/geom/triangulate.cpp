#include "dnp/geom/triangulate.hpp"

namespace dnp::geom {

namespace {

// 2D cross product at raw scale 2^64, in 128-bit so a large sector cannot
// overflow a term before it is compared.
inline i128 cross_raw(fx2 o, fx2 a, fx2 b) noexcept {
    return (static_cast<i128>(a.x.raw) - o.x.raw) * (static_cast<i128>(b.y.raw) - o.y.raw) -
           (static_cast<i128>(a.y.raw) - o.y.raw) * (static_cast<i128>(b.x.raw) - o.x.raw);
}

}  // namespace

i128 polygon_area_2x(const fx2* pts, u32 n) noexcept {
    i128 acc = 0;
    for (u32 i = 0, j = n - 1; i < n; j = i++) {
        acc += (static_cast<i128>(pts[j].x.raw) * pts[i].y.raw) -
               (static_cast<i128>(pts[i].x.raw) * pts[j].y.raw);
    }
    return acc;
}

bool point_in_triangle(fx2 a, fx2 b, fx2 c, fx2 p) noexcept {
    // For a counter-clockwise triangle, an interior point is left of every
    // edge. `>= 0` rather than `> 0` so the boundary counts as inside.
    const i128 d1 = cross_raw(a, b, p);
    const i128 d2 = cross_raw(b, c, p);
    const i128 d3 = cross_raw(c, a, p);
    return d1 >= 0 && d2 >= 0 && d3 >= 0;
}

TriangulateStatus triangulate(const fx2* pts, u32 n, TriangleList& out) noexcept {
    out.count = 0;
    if (n < 3) return TriangulateStatus::TooFewPoints;
    if (n > MAX_POLY_POINTS) return TriangulateStatus::TooManyPoints;

    // Working index ring. Normalise winding to counter-clockwise here rather
    // than requiring it of the caller: an editor emits whichever order the user
    // drew in, and a silently-inverted sector produces outward normals pointing
    // inward, which reads as "the walls do not collide" much later.
    u16 ring[MAX_POLY_POINTS];
    const bool ccw = polygon_area_2x(pts, n) > 0;
    for (u32 i = 0; i < n; ++i) {
        ring[i] = static_cast<u16>(ccw ? i : (n - 1 - i));
    }

    u32 remaining = n;

    // Guard against a polygon that is not simple. Without it, a self-
    // intersecting input spins here forever -- and a runtime update_shape()
    // call is a place where hostile or merely buggy data can arrive.
    u32 without_progress = 0;
    u32 cursor = 0;

    while (remaining > 3) {
        if (without_progress > remaining) {
            out.count = 0;
            return TriangulateStatus::DegenerateOrSelfIntersecting;
        }

        const u32 ip = (cursor + remaining - 1) % remaining;
        const u32 ic = cursor % remaining;
        const u32 in = (cursor + 1) % remaining;

        const fx2 a = pts[ring[ip]];
        const fx2 b = pts[ring[ic]];
        const fx2 c = pts[ring[in]];

        // Convex corner? A reflex or collinear corner cannot be an ear.
        // Collinear (== 0) is excluded deliberately: clipping it would emit a
        // zero-area triangle, which renders as nothing and confuses anything
        // that later computes a normal from it.
        if (cross_raw(a, b, c) <= 0) {
            ++cursor;
            ++without_progress;
            continue;
        }

        // ...and does the candidate ear contain any other vertex? Checking all
        // of them rather than only the reflex ones is O(n) instead of O(r), and
        // for polygons this small the simpler loop is worth more than the
        // saving.
        bool contains = false;
        for (u32 k = 0; k < remaining; ++k) {
            if (k == ip || k == ic || k == in) continue;
            if (point_in_triangle(a, b, c, pts[ring[k]])) { contains = true; break; }
        }
        if (contains) {
            ++cursor;
            ++without_progress;
            continue;
        }

        out.indices[out.count * 3 + 0] = ring[ip];
        out.indices[out.count * 3 + 1] = ring[ic];
        out.indices[out.count * 3 + 2] = ring[in];
        ++out.count;

        // Remove the clipped vertex, preserving order.
        for (u32 k = ic; k + 1 < remaining; ++k) ring[k] = ring[k + 1];
        --remaining;

        // Resume scanning from the same position rather than restarting at 0.
        // Either is deterministic; this one keeps the walk local to where the
        // polygon just changed, which finds the next ear sooner.
        if (cursor >= remaining) cursor = 0;
        without_progress = 0;
    }

    out.indices[out.count * 3 + 0] = ring[0];
    out.indices[out.count * 3 + 1] = ring[1];
    out.indices[out.count * 3 + 2] = ring[2];
    ++out.count;

    return TriangulateStatus::Ok;
}

}  // namespace dnp::geom
