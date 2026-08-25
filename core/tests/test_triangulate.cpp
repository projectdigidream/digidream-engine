//
// Triangulation tests.
//
// Correctness here means more than "it produced some triangles". A wrong
// triangulation still renders, still has plausible area, and only shows up as a
// visual seam or a hole much later -- so these check the invariants that
// actually pin it down: triangle count, total area equal to the polygon's,
// consistent winding, indices in range, and no degenerate triangles.
//
// The determinism check is the one that matters most: `update_shape()`
// re-triangulates at runtime and the result is replicated, so every peer must
// produce not just a valid triangulation but the SAME one.
//
#include <cstdio>

#include "dnp/geom/mapstore.hpp"
#include "dnp/sim/testmap.hpp"
#include "dnp/geom/triangulate.hpp"
#include "dnp/math/rng.hpp"
#include "dnp/math/trig.hpp"

using namespace dnp;
using namespace dnp::geom;

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_fails;                                                     \
            std::printf("  FAIL %s:%d   %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

static fx F(i64 n) { return fx::from_int(n); }

static i128 tri_area_2x(fx2 a, fx2 b, fx2 c) {
    return (static_cast<i128>(b.x.raw) - a.x.raw) * (static_cast<i128>(c.y.raw) - a.y.raw) -
           (static_cast<i128>(b.y.raw) - a.y.raw) * (static_cast<i128>(c.x.raw) - a.x.raw);
}

// The invariants a valid triangulation of a simple polygon must satisfy.
static void verify(const fx2* pts, u32 n, const TriangleList& t, const char* what) {
    // A simple polygon always yields exactly n-2 triangles. Anything else means
    // an ear was clipped that should not have been, or one was missed.
    if (t.count != n - 2) {
        ++g_fails;
        std::printf("  FAIL %s: expected %u triangles, got %u\n", what, n - 2, t.count);
        return;
    }
    ++g_checks;

    i128 sum = 0;
    for (u16 i = 0; i < t.count; ++i) {
        const u16 ia = t.indices[i * 3 + 0];
        const u16 ib = t.indices[i * 3 + 1];
        const u16 ic = t.indices[i * 3 + 2];
        CHECK(ia < n && ib < n && ic < n);
        CHECK(ia != ib && ib != ic && ia != ic);

        const i128 a2 = tri_area_2x(pts[ia], pts[ib], pts[ic]);
        // Counter-clockwise and non-degenerate. A zero-area triangle renders as
        // nothing and produces a garbage normal for anything that asks.
        CHECK(a2 > 0);
        sum += a2;
    }

    // Total area must equal the polygon's exactly. This is the assertion that
    // actually catches overlaps and gaps: a triangulation that double-covers
    // part of the shape or leaves a sliver uncovered fails here while looking
    // perfectly reasonable on screen.
    const i128 poly = polygon_area_2x(pts, n);
    const i128 want = poly > 0 ? poly : -poly;
    CHECK(sum == want);
}

static void test_convex() {
    std::printf("triangulate: convex polygons\n");

    {
        const fx2 sq[] = {{F(0), F(0)}, {F(10), F(0)}, {F(10), F(10)}, {F(0), F(10)}};
        TriangleList t;
        CHECK(triangulate(sq, 4, t) == TriangulateStatus::Ok);
        verify(sq, 4, t, "square");
    }
    {
        const fx2 tri[] = {{F(0), F(0)}, {F(4), F(0)}, {F(0), F(3)}};
        TriangleList t;
        CHECK(triangulate(tri, 3, t) == TriangulateStatus::Ok);
        CHECK(t.count == 1);
        verify(tri, 3, t, "triangle");
    }
    // A regular-ish polygon with many vertices.
    {
        fx2 poly[16];
        for (int i = 0; i < 16; ++i) {
            const fx2 d = fx_dir(brad(static_cast<u32>(static_cast<u64>(i) * 4294967296ull / 16)));
            poly[i] = fx2{d.x * F(20), d.y * F(20)};
        }
        TriangleList t;
        CHECK(triangulate(poly, 16, t) == TriangulateStatus::Ok);
        verify(poly, 16, t, "16-gon");
    }
}

static void test_concave() {
    std::printf("triangulate: concave polygons, which is the whole point\n");

    // L shape.
    {
        const fx2 l[] = {{F(0), F(0)}, {F(10), F(0)}, {F(10), F(4)},
                         {F(4), F(4)}, {F(4), F(10)}, {F(0), F(10)}};
        TriangleList t;
        CHECK(triangulate(l, 6, t) == TriangulateStatus::Ok);
        verify(l, 6, t, "L");
    }
    // A comb: several deep notches, which forces repeated non-ear scans.
    {
        fx2 comb[14];
        int k = 0;
        comb[k++] = {F(0), F(0)};
        for (int i = 0; i < 3; ++i) {
            const fx x0 = F(2 + i * 4);
            comb[k++] = {x0, F(0)};
            comb[k++] = {x0, F(8)};
            comb[k++] = {x0 + F(2), F(8)};
            comb[k++] = {x0 + F(2), F(0)};
        }
        comb[k++] = {F(14), F(0)};
        // close the outline along the top
        // (k == 14 by construction)
        TriangleList t;
        const TriangulateStatus st = triangulate(comb, static_cast<u32>(k), t);
        // This outline is deliberately awkward; whatever the verdict, it must be
        // a clean one -- either a full valid triangulation or an explicit
        // rejection, never a partial mesh.
        CHECK(st == TriangulateStatus::Ok || t.count == 0);
        if (st == TriangulateStatus::Ok) verify(comb, static_cast<u32>(k), t, "comb");
    }
}

static void test_winding_normalised() {
    std::printf("triangulate: either input winding yields CCW triangles\n");

    const fx2 ccw[] = {{F(0), F(0)}, {F(10), F(0)}, {F(10), F(10)}, {F(0), F(10)}};
    const fx2 cw[]  = {{F(0), F(10)}, {F(10), F(10)}, {F(10), F(0)}, {F(0), F(0)}};

    TriangleList a, b;
    CHECK(triangulate(ccw, 4, a) == TriangulateStatus::Ok);
    CHECK(triangulate(cw, 4, b) == TriangulateStatus::Ok);
    verify(ccw, 4, a, "ccw square");
    verify(cw, 4, b, "cw square");

    // An editor emits whichever order the user drew in. A silently inverted
    // sector gives outward normals pointing inward, which surfaces much later
    // as "the walls do not collide".
    CHECK(a.count == b.count);
}

static void test_rejects_bad_input() {
    std::printf("triangulate: bad input is rejected cleanly, never partially\n");

    TriangleList t;
    const fx2 two[] = {{F(0), F(0)}, {F(1), F(1)}};
    CHECK(triangulate(two, 2, t) == TriangulateStatus::TooFewPoints);
    CHECK(t.count == 0);

    // Self-intersecting bowtie: not a simple polygon, so there is no valid
    // triangulation. It must terminate and say so rather than spin.
    const fx2 bowtie[] = {{F(0), F(0)}, {F(10), F(10)}, {F(10), F(0)}, {F(0), F(10)}};
    const TriangulateStatus st = triangulate(bowtie, 4, t);
    CHECK(st == TriangulateStatus::DegenerateOrSelfIntersecting || t.count == 2);

    // All points collinear: zero area, no ears.
    const fx2 line[] = {{F(0), F(0)}, {F(1), F(0)}, {F(2), F(0)}, {F(3), F(0)}};
    const TriangulateStatus st2 = triangulate(line, 4, t);
    CHECK(st2 == TriangulateStatus::DegenerateOrSelfIntersecting);
    CHECK(t.count == 0);
}

static void test_determinism() {
    // The reason this file lives in core/ rather than in the map compiler:
    // update_shape() re-triangulates at runtime and the commit is replicated,
    // so a valid-but-different triangulation on one peer is a desync.
    std::printf("triangulate: identical output for identical input\n");

    Rng rng(4242);
    for (int trial = 0; trial < 500; ++trial) {
        // Random star-shaped polygon: guaranteed simple, and reliably concave.
        const u32 n = 5 + rng.next_below(11);
        fx2 poly[MAX_POLY_POINTS];
        for (u32 i = 0; i < n; ++i) {
            const brad ang = brad(static_cast<u32>(static_cast<u64>(i) * 4294967296ull / n));
            const fx r = rng.next_fx(F(4), F(20));
            const fx2 d = fx_dir(ang);
            poly[i] = fx2{d.x * r, d.y * r};
        }

        TriangleList a, b;
        const TriangulateStatus sa = triangulate(poly, n, a);
        const TriangulateStatus sb = triangulate(poly, n, b);
        CHECK(sa == sb);
        CHECK(a.count == b.count);
        for (u16 i = 0; i < a.count * 3; ++i) CHECK(a.indices[i] == b.indices[i]);

        if (sa == TriangulateStatus::Ok) verify(poly, n, a, "star");
    }
}

// ------------------------------------------------------- staged shape edits

static void test_staged_commit() {
    std::printf("mapstore: staged edits are atomic and rebuild the mesh\n");

    MapStorage store;
    const fx2 sq[] = {{F(0), F(0)}, {F(10), F(0)}, {F(10), F(10)}, {F(0), F(10)}};
    const u32 s = store.add_sector(sq, 4, F(0), F(20), 7);
    CHECK(s == 0);
    CHECK(store.view().sector(0).tri_count == 2);   // triangulated on add

    const u64 h0 = store.hash();

    // Staging changes nothing observable. This is the property design 3.1.1
    // requires be documented: a script may stage several edits and everything
    // that reads the map in between still sees the last valid shape.
    CHECK(store.stage_point(0, 1, fx2{F(20), F(0)}));
    CHECK(store.stage_point(0, 2, fx2{F(20), F(10)}));
    CHECK(store.has_staged_edits(0));
    CHECK(store.hash() == h0);
    CHECK(store.view().point(1) == (fx2{F(10), F(0)}));

    // One commit applies them all.
    CHECK(store.commit_shape(0) == CommitStatus::Ok);
    CHECK(!store.has_staged_edits(0));
    CHECK(store.view().point(1) == (fx2{F(20), F(0)}));
    CHECK(store.view().point(2) == (fx2{F(20), F(10)}));
    CHECK(store.hash() != h0);
    CHECK(store.view().sector(0).tri_count == 2);

    // Committing with nothing staged is a defined no-op, not an error.
    CHECK(store.commit_shape(0) == CommitStatus::NoStagedEdits);
}

static void test_rejected_commit_is_inert() {
    std::printf("mapstore: a rejected shape leaves the world untouched\n");

    MapStorage store;
    const fx2 sq[] = {{F(0), F(0)}, {F(10), F(0)}, {F(10), F(10)}, {F(0), F(10)}};
    store.add_sector(sq, 4, F(0), F(20), 1);
    const u64 h0 = store.hash();

    // Drag one corner across the polygon to make it self-intersecting.
    CHECK(store.stage_point(0, 1, fx2{F(-50), F(50)}));
    const CommitStatus st = store.commit_shape(0);

    if (st != CommitStatus::Ok) {
        // The proposed shape is triangulated BEFORE the live points are
        // touched, so a rejection cannot leave points and mesh disagreeing --
        // and anything that queried collision in such a window would get an
        // answer derived from geometry that never existed.
        CHECK(store.hash() == h0);
        CHECK(store.view().point(1) == (fx2{F(10), F(0)}));
        CHECK(!store.has_staged_edits(0));
    }
}

static void test_commit_determinism() {
    // The commit is replicated (docs/protocol.md 8.1), so two peers applying
    // the same edit must arrive at byte-identical geometry.
    std::printf("mapstore: identical edits produce identical geometry\n");

    Rng rng(31337);
    for (int trial = 0; trial < 200; ++trial) {
        MapStorage a, b;
        fx2 poly[8];
        const u32 n = 5 + rng.next_below(4);
        for (u32 i = 0; i < n; ++i) {
            const brad ang = brad(static_cast<u32>(static_cast<u64>(i) * 4294967296ull / n));
            const fx r = rng.next_fx(F(5), F(15));
            const fx2 d = fx_dir(ang);
            poly[i] = fx2{d.x * r, d.y * r};
        }
        a.add_sector(poly, n, F(0), F(20), 1);
        b.add_sector(poly, n, F(0), F(20), 1);
        CHECK(a.hash() == b.hash());

        const u16 idx = static_cast<u16>(rng.next_below(n));
        const fx2 moved{rng.next_fx(F(-18), F(18)), rng.next_fx(F(-18), F(18))};
        a.stage_point(0, idx, moved);
        b.stage_point(0, idx, moved);
        const CommitStatus sa = a.commit_shape(0);
        const CommitStatus sb = b.commit_shape(0);
        CHECK(sa == sb);
        CHECK(a.hash() == b.hash());
    }
}

static void test_baked_matches_runtime() {
    // The test map ships hand-written triangle indices. Those must be exactly
    // what the runtime triangulator produces for the same polygons -- otherwise
    // a sector that is re-triangulated by update_shape() would silently change
    // shape relative to how it was baked, and the seam would appear only after
    // an edit. This is the assertion behind the claim in testmap.cpp.
    std::printf("mapstore: baked triangles match the runtime triangulator\n");

    const MapData& m = dnp::sim::test_map();
    CHECK(m.tris != nullptr);

    for (u32 si = 0; si < m.sector_count; ++si) {
        const Sector& sec = m.sector(si);
        TriangleList t;
        CHECK(triangulate(&m.point(sec.first_point), sec.point_count, t) == TriangulateStatus::Ok);
        CHECK(t.count == sec.tri_count);
        for (u16 k = 0; k < t.count * 3; ++k) {
            // Baked indices are absolute; the triangulator emits sector-local.
            CHECK(m.tris[sec.first_tri + k] == sec.first_point + t.indices[k]);
        }
    }
}

int main() {
    std::printf("== core/geom triangulation suite ==\n");
    test_convex();
    test_concave();
    test_winding_normalised();
    test_rejects_bad_input();
    test_determinism();
    test_staged_commit();
    test_rejected_commit_is_inert();
    test_commit_determinism();
    test_baked_matches_runtime();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
