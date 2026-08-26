#pragma once
//
// Deterministic ear-clipping triangulation.
//
// Design §3.1.1 presents triangulation as an edit-time step, and mostly it is:
// the map compiler bakes triangle indices into the map file so the runtime does
// not pay for them. But `update_shape()` re-triangulates at RUNTIME, and that
// commit is replicated, which means the server and every client must produce
// not merely a valid triangulation but the SAME one. So this lives in core/,
// under the determinism gate, and `tools/mapc` calls it rather than carrying a
// second implementation that would agree right up until it did not.
//
// Every predicate is exact: convexity and point-in-triangle are decided with
// 128-bit cross products and no division. A polygon vertex that sits exactly on
// another edge is the case that would otherwise be decided by rounding, and it
// is precisely the case a level editor produces constantly.
//
#include "dnp/math/fixed.hpp"
#include "dnp/math/vec.hpp"

namespace dnp::geom {

using namespace dnp;

// One loop's worth of points. The cap bounds the O(n^2) inner scan and makes
// the work per commit predictable.
inline constexpr u32 MAX_POLY_POINTS = 96;

// A polygon WITH holes is triangulated by bridging each hole into the outer
// loop, which produces one simple polygon carrying every loop's vertices plus
// two duplicates per bridge. This bounds that merged form.
inline constexpr u32 MAX_MERGED_POINTS = 256;
inline constexpr u32 MAX_POLY_TRIS     = MAX_MERGED_POINTS - 2;
inline constexpr u32 MAX_POLY_HOLES    = 16;

enum class TriangulateStatus : u8 {
    Ok = 0,
    TooFewPoints,      // fewer than 3
    TooManyPoints,     // exceeds MAX_POLY_POINTS
    DegenerateOrSelfIntersecting,   // no ear found; the input is not a simple polygon
};

struct TriangleList {
    u16 indices[MAX_POLY_TRIS * 3]{};   // into the caller's point array
    u16 count = 0;                      // triangles, not indices
};

// Twice the signed area, in 128-bit at raw scale 2^64.
// Positive means counter-clockwise. Exact -- this decides winding, and a
// mis-detected winding turns every normal inside out.
i128 polygon_area_2x(const fx2* pts, u32 n) noexcept;

// Triangulate a simple polygon.
//
// Winding is normalised internally, so the caller may supply either order and
// the emitted triangles are always counter-clockwise. Indices are into `pts`.
//
// On failure the output is left empty and a status is returned rather than a
// partial mesh: a half-triangulated sector is worse than none, because it looks
// plausible. Design §3.1.1 says a failed commit keeps the last valid
// triangulation, and that is only possible if failure is unambiguous.
TriangulateStatus triangulate(const fx2* pts, u32 n, TriangleList& out) noexcept;

// One loop, as a range of indices into a shared point array.
struct PolyLoop {
    u16 first = 0;
    u16 count = 0;
};

// Triangulate a polygon with holes (design §3.1, relaxing "no holes").
//
// Doom's model needs this and ours did not have it: a room with a pit in it is
// an ANNULUS -- the room's floor genuinely does not exist over the pit -- and
// no arrangement of two simple polygons expresses that. Nesting one sector
// inside another gets a raised PLATFORM right, because the room's floor does
// exist under a shelf; a pit is the opposite case and needs the hole to be real
// geometry, in the mesh as much as in the collision.
//
// Each hole is bridged into the outer loop by a cut to a mutually visible
// vertex, giving a single simple polygon the existing ear clipper handles. The
// bridge's visibility is TESTED rather than chosen by a heuristic: candidate
// vertices are tried nearest-first, and the first whose cut crosses no edge and
// stays inside the polygon wins. Exact predicates throughout, so every target
// bridges identically -- update_shape() re-triangulates at runtime and the
// result is replicated.
TriangulateStatus triangulate_with_holes(const fx2* pts, u32 outer_n,
                                         const PolyLoop* holes, u32 hole_count,
                                         TriangleList& out) noexcept;

// Is `p` inside (or on the boundary of) the counter-clockwise triangle a,b,c?
// Exact. Boundary counts as inside, which is the conservative choice for ear
// detection: treating a touching vertex as outside would clip an ear that
// swallows it.
bool point_in_triangle(fx2 a, fx2 b, fx2 c, fx2 p) noexcept;

}  // namespace dnp::geom
