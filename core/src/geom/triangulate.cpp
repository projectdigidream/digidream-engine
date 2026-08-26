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

namespace {

// Ear-clip an index sequence that is ALREADY counter-clockwise.
//
// Split out because a polygon with holes is triangulated by bridging its holes
// into the outer loop and clipping the merged sequence -- the same clipper, on a
// vertex list that is no longer a contiguous range and contains deliberate
// duplicates at each bridge.
TriangulateStatus ear_clip(const fx2* pts, const u16* seq, u32 n,
                           TriangleList& out) noexcept {
    out.count = 0;
    if (n < 3) return TriangulateStatus::TooFewPoints;
    if (n > MAX_MERGED_POINTS) return TriangulateStatus::TooManyPoints;

    u16 ring[MAX_MERGED_POINTS];
    for (u32 i = 0; i < n; ++i) ring[i] = seq[i];

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
            const fx2 q = pts[ring[k]];
            // Skipped by COORDINATE as well as by position.
            //
            // Bridging a hole duplicates two vertices -- the cut is traversed
            // in both directions -- so a vertex identical to one of the ear's
            // own corners appears elsewhere in the ring. point_in_triangle
            // counts the boundary as inside, so without this every ear contains
            // "another" vertex that is really one of its own corners, no ear is
            // ever found, and a polygon with holes is reported as
            // self-intersecting. Which is exactly what it did.
            if (q == a || q == b || q == c) continue;
            if (point_in_triangle(a, b, c, q)) { contains = true; break; }
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

// Orientation of c relative to the directed line a->b. Exact.
inline i128 orient(fx2 a, fx2 b, fx2 c) noexcept { return cross_raw(a, b, c); }

// Is p strictly between a and b, given it is already known to be collinear?
inline bool strictly_between(fx2 a, fx2 b, fx2 p) noexcept {
    if (p == a || p == b) return false;
    const i64 lox = a.x.raw < b.x.raw ? a.x.raw : b.x.raw;
    const i64 hix = a.x.raw < b.x.raw ? b.x.raw : a.x.raw;
    const i64 loy = a.y.raw < b.y.raw ? a.y.raw : b.y.raw;
    const i64 hiy = a.y.raw < b.y.raw ? b.y.raw : a.y.raw;
    return p.x.raw >= lox && p.x.raw <= hix && p.y.raw >= loy && p.y.raw <= hiy;
}

// Do two segments meet anywhere other than at a shared endpoint?
//
// A bridge is allowed to touch the polygon at its own two endpoints and nowhere
// else. Touching mid-edge, or overlapping along one, would make the merged
// polygon non-simple, and the ear clipper would then refuse it -- correctly, but
// with no way to say which hole caused it.
bool blocks (fx2 a, fx2 b, fx2 c, fx2 d) noexcept {
    const i128 d1 = orient(c, d, a);
    const i128 d2 = orient(c, d, b);
    const i128 d3 = orient(a, b, c);
    const i128 d4 = orient(a, b, d);

    if (((d1 > 0) != (d2 > 0)) && ((d1 != 0) && (d2 != 0)) &&
        ((d3 > 0) != (d4 > 0)) && ((d3 != 0) && (d4 != 0))) {
        return true;
    }
    if (d1 == 0 && strictly_between(c, d, a)) return true;
    if (d2 == 0 && strictly_between(c, d, b)) return true;
    if (d3 == 0 && strictly_between(a, b, c)) return true;
    if (d4 == 0 && strictly_between(a, b, d)) return true;
    return false;
}

// Crossing-number test over one index sequence.
bool point_in_seq (const fx2* pts, const u16* seq, u32 n, fx2 p) noexcept {
    bool inside = false;
    for (u32 i = 0, j = n - 1; i < n; j = i++) {
        const fx2& pi = pts[seq[i]];
        const fx2& pj = pts[seq[j]];
        if ((pi.y > p.y) == (pj.y > p.y)) continue;
        const i128 dy = static_cast<i128>(pj.y.raw) - pi.y.raw;
        const i128 lhs = (static_cast<i128>(p.x.raw) - pi.x.raw) * dy;
        const i128 rhs = (static_cast<i128>(pj.x.raw) - pi.x.raw) *
                         (static_cast<i128>(p.y.raw) - pi.y.raw);
        if (dy > 0 ? (lhs < rhs) : (lhs > rhs)) inside = !inside;
    }
    return inside;
}

}  // namespace

TriangulateStatus triangulate(const fx2* pts, u32 n, TriangleList& out) noexcept {
    out.count = 0;
    if (n < 3) return TriangulateStatus::TooFewPoints;
    if (n > MAX_POLY_POINTS) return TriangulateStatus::TooManyPoints;

    // Normalise winding to counter-clockwise here rather than requiring it of
    // the caller: an editor emits whichever order the user drew in, and a
    // silently-inverted sector produces outward normals pointing inward, which
    // reads as "the walls do not collide" much later.
    u16 seq[MAX_POLY_POINTS];
    const bool ccw = polygon_area_2x(pts, n) > 0;
    for (u32 i = 0; i < n; ++i) seq[i] = static_cast<u16>(ccw ? i : (n - 1 - i));
    return ear_clip(pts, seq, n, out);
}

TriangulateStatus triangulate_with_holes(const fx2* pts, u32 outer_n,
                                         const PolyLoop* holes, u32 hole_count,
                                         TriangleList& out) noexcept {
    out.count = 0;
    if (hole_count == 0) return triangulate(pts, outer_n, out);
    if (outer_n < 3) return TriangulateStatus::TooFewPoints;
    if (outer_n > MAX_POLY_POINTS) return TriangulateStatus::TooManyPoints;
    if (hole_count > MAX_POLY_HOLES) return TriangulateStatus::TooManyPoints;

    // The outer loop, counter-clockwise.
    u16 seq[MAX_MERGED_POINTS];
    u32 seq_n = 0;
    {
        const bool ccw = polygon_area_2x(pts, outer_n) > 0;
        for (u32 i = 0; i < outer_n; ++i) {
            seq[seq_n++] = static_cast<u16>(ccw ? i : (outer_n - 1 - i));
        }
    }

    // Holes are bridged in order of their rightmost vertex, rightmost first.
    // Any fixed order works; this one is the classic choice because a hole
    // further right is more likely to see the outer boundary directly, so the
    // easy bridges are made while the polygon is still simple.
    u16 order[MAX_POLY_HOLES];
    for (u32 i = 0; i < hole_count; ++i) order[i] = static_cast<u16>(i);
    for (u32 i = 1; i < hole_count; ++i) {
        for (u32 j = i; j > 0; --j) {
            const PolyLoop& A = holes[order[j]];
            const PolyLoop& B = holes[order[j - 1]];
            fx ax = pts[A.first].x;
            for (u16 k = 1; k < A.count; ++k) if (pts[A.first + k].x > ax) ax = pts[A.first + k].x;
            fx bx = pts[B.first].x;
            for (u16 k = 1; k < B.count; ++k) if (pts[B.first + k].x > bx) bx = pts[B.first + k].x;
            // Strictly greater, so equal rightmost extents keep their original
            // order and the sort is stable on every target.
            if (!(ax > bx)) break;
            const u16 t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    }

    bool merged[MAX_POLY_HOLES] = {};

    for (u32 oi = 0; oi < hole_count; ++oi) {
        const u32 h = order[oi];
        const PolyLoop& hole = holes[h];
        if (hole.count < 3 || hole.count > MAX_POLY_POINTS) {
            out.count = 0;
            return TriangulateStatus::DegenerateOrSelfIntersecting;
        }

        // The hole, CLOCKWISE -- opposite the outer loop, which is what makes
        // the bridged result a single simple counter-clockwise polygon.
        u16 hseq[MAX_POLY_POINTS];
        {
            const bool hccw = polygon_area_2x(pts + hole.first, hole.count) > 0;
            for (u32 i = 0; i < hole.count; ++i) {
                hseq[i] = static_cast<u16>(hole.first + (hccw ? (hole.count - 1 - i) : i));
            }
        }

        // M: the hole's rightmost vertex, ties to the earliest position.
        u32 mi = 0;
        for (u32 i = 1; i < hole.count; ++i) {
            if (pts[hseq[i]].x > pts[hseq[mi]].x) mi = i;
        }
        const fx2 M = pts[hseq[mi]];

        // Candidate bridge targets, nearest first. Visibility is TESTED rather
        // than chosen by an angle heuristic: the first candidate whose cut
        // crosses nothing and stays inside the polygon is provably usable, and
        // there is no case left where a heuristic picks a vertex that is not.
        u16 cand[MAX_MERGED_POINTS];
        for (u32 i = 0; i < seq_n; ++i) cand[i] = static_cast<u16>(i);
        for (u32 i = 1; i < seq_n; ++i) {
            for (u32 j = i; j > 0; --j) {
                const fx2 A = pts[seq[cand[j]]];
                const fx2 B = pts[seq[cand[j - 1]]];
                const u128 da = len_sq_raw(fx2{A.x - M.x, A.y - M.y});
                const u128 db = len_sq_raw(fx2{B.x - M.x, B.y - M.y});
                if (!(da < db)) break;      // stable on ties
                const u16 t = cand[j]; cand[j] = cand[j - 1]; cand[j - 1] = t;
            }
        }

        i32 chosen = -1;
        for (u32 ci = 0; ci < seq_n && chosen < 0; ++ci) {
            const u32 pi = cand[ci];
            const fx2 P = pts[seq[pi]];
            if (P == M) continue;

            bool ok = true;
            for (u32 e = 0; e < seq_n && ok; ++e) {
                const fx2 a = pts[seq[e]];
                const fx2 b = pts[seq[(e + 1) % seq_n]];
                if (blocks(M, P, a, b)) ok = false;
            }
            for (u32 e = 0; e < hole.count && ok; ++e) {
                const fx2 a = pts[hseq[e]];
                const fx2 b = pts[hseq[(e + 1) % hole.count]];
                if (blocks(M, P, a, b)) ok = false;
            }
            // Holes not yet bridged are still separate loops, and a cut through
            // one of them would enclose it on the wrong side.
            for (u32 g = 0; g < hole_count && ok; ++g) {
                if (g == h || merged[g]) continue;
                const PolyLoop& other = holes[g];
                for (u32 e = 0; e < other.count && ok; ++e) {
                    const fx2 a = pts[other.first + e];
                    const fx2 b = pts[other.first + (e + 1) % other.count];
                    if (blocks(M, P, a, b)) ok = false;
                }
            }
            if (!ok) continue;

            // The cut must also lie INSIDE the polygon rather than crossing the
            // void outside it, which touching no edge does not rule out.
            const fx2 mid{(M.x + P.x) / 2, (M.y + P.y) / 2};
            if (!point_in_seq(pts, seq, seq_n, mid)) continue;
            bool in_hole = false;
            for (u32 g = 0; g < hole_count && !in_hole; ++g) {
                if (merged[g]) continue;
                const PolyLoop& other = holes[g];
                u16 tmp[MAX_POLY_POINTS];
                for (u32 e = 0; e < other.count; ++e) tmp[e] = static_cast<u16>(other.first + e);
                if (point_in_seq(pts, tmp, other.count, mid)) in_hole = true;
            }
            if (in_hole) continue;

            chosen = static_cast<i32>(pi);
        }

        if (chosen < 0) {
            out.count = 0;
            return TriangulateStatus::DegenerateOrSelfIntersecting;
        }

        // Splice: ... P, M, (the rest of the hole), M, P, ...
        const u32 insert = static_cast<u32>(chosen) + 1;
        const u32 added = hole.count + 2;
        if (seq_n + added > MAX_MERGED_POINTS) {
            out.count = 0;
            return TriangulateStatus::TooManyPoints;
        }
        for (u32 i = seq_n; i > insert; --i) seq[i - 1 + added] = seq[i - 1];
        for (u32 i = 0; i < hole.count; ++i) {
            seq[insert + i] = hseq[(mi + i) % hole.count];
        }
        seq[insert + hole.count] = hseq[mi];
        seq[insert + hole.count + 1] = seq[static_cast<u32>(chosen)];
        seq_n += added;
        merged[h] = true;
    }

    return ear_clip(pts, seq, seq_n, out);
}

}  // namespace dnp::geom
