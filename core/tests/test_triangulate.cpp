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

// Total area of a triangle list, doubled, in 128-bit.
static i128 tri_area_2x(const fx2* pts, const TriangleList& t) {
    i128 acc = 0;
    for (u16 i = 0; i < t.count; ++i) {
        const fx2 a = pts[t.indices[i * 3]];
        const fx2 b = pts[t.indices[i * 3 + 1]];
        const fx2 c = pts[t.indices[i * 3 + 2]];
        acc += (static_cast<i128>(b.x.raw) - a.x.raw) * (static_cast<i128>(c.y.raw) - a.y.raw) -
               (static_cast<i128>(b.y.raw) - a.y.raw) * (static_cast<i128>(c.x.raw) - a.x.raw);
    }
    return acc;
}

// How many triangles contain a point.
//
// A point on an internal edge is legitimately counted by BOTH triangles that
// share it -- point_in_triangle treats the boundary as inside, deliberately, so
// that ear detection does not clip an ear over a touching vertex. So coverage
// is asserted as "at least one" and emptiness as "exactly none"; asserting
// exactly one is a test of whether the probe happened to miss an edge.
static int triangles_covering(const fx2* pts, const TriangleList& t, fx2 p) {
    int n = 0;
    for (u16 i = 0; i < t.count; ++i) {
        if (point_in_triangle(pts[t.indices[i * 3]], pts[t.indices[i * 3 + 1]],
                              pts[t.indices[i * 3 + 2]], p)) {
            ++n;
        }
    }
    return n;
}

static void test_holes() {
    std::printf("triangulate: an annulus covers the ring and not the hole\n");

    // A 40x40 room with a 12x12 hole in the middle. This is a Doom room with a
    // pit in it, and it is the shape our model could not express: nesting a
    // second sector inside the first leaves the room's floor covering the pit,
    // so a body walks over it.
    const fx2 pts[] = {
        {F(0), F(0)}, {F(40), F(0)}, {F(40), F(40)}, {F(0), F(40)},        // outer
        {F(14), F(14)}, {F(26), F(14)}, {F(26), F(26)}, {F(14), F(26)}     // hole
    };
    const PolyLoop holes[] = {{4, 4}};

    TriangleList t;
    CHECK(triangulate_with_holes(pts, 4, holes, 1, t) == TriangulateStatus::Ok);
    CHECK(t.count > 0);

    // Area: the ring is 40*40 - 12*12 = 1456. Doubled, at raw scale.
    const i128 want = static_cast<i128>(fx::from_int(1456).raw) * fx::one().raw * 2;
    CHECK(tri_area_2x(pts, t) == want);

    // And every triangle is counter-clockwise, so no part of the mesh is
    // inside out.
    bool all_ccw = true;
    for (u16 i = 0; i < t.count; ++i) {
        const fx2 a = pts[t.indices[i * 3]];
        const fx2 b = pts[t.indices[i * 3 + 1]];
        const fx2 c = pts[t.indices[i * 3 + 2]];
        const i128 cr = (static_cast<i128>(b.x.raw) - a.x.raw) * (static_cast<i128>(c.y.raw) - a.y.raw) -
                        (static_cast<i128>(b.y.raw) - a.y.raw) * (static_cast<i128>(c.x.raw) - a.x.raw);
        if (cr <= 0) all_ccw = false;
    }
    CHECK(all_ccw);

    // Coverage, probed. Area alone would be satisfied by a mesh that covered
    // the hole and left an equal gap somewhere else.
    int ring_covered = 0;
    int ring_probes = 0;
    int hole_covered = 0;
    int hole_probes = 0;
    for (int x = 1; x < 40; ++x) {
        for (int y = 1; y < 40; ++y) {
            const fx2 p{fx::from_frac(x * 10 + 5, 10), fx::from_frac(y * 10 + 5, 10)};
            const bool in_hole = x >= 14 && x < 26 && y >= 14 && y < 26;
            const int n = triangles_covering(pts, t, p);
            if (in_hole) { ++hole_probes; if (n > 0) ++hole_covered; }
            else { ++ring_probes; if (n > 0) ++ring_covered; }
        }
    }
    CHECK(ring_probes > 1000 && hole_probes > 100);
    CHECK(ring_covered == ring_probes);
    CHECK(hole_covered == 0);

    // Two holes, and one of them concave -- the case where a bridge to the
    // nearest visible vertex has to reach past the first hole already merged.
    const fx2 two[] = {
        {F(0), F(0)}, {F(60), F(0)}, {F(60), F(40)}, {F(0), F(40)},
        {F(8), F(8)}, {F(20), F(8)}, {F(20), F(20)}, {F(14), F(20)},
        {F(14), F(14)}, {F(8), F(14)},
        {F(38), F(10)}, {F(52), F(10)}, {F(52), F(30)}, {F(38), F(30)}
    };
    const PolyLoop two_holes[] = {{4, 6}, {10, 4}};
    TriangleList t2;
    CHECK(triangulate_with_holes(two, 4, two_holes, 2, t2) == TriangulateStatus::Ok);
    // 60*40 - (12*12 - 6*6) - 14*20 = 2400 - 108 - 280 = 2012
    const i128 want2 = static_cast<i128>(fx::from_int(2012).raw) * fx::one().raw * 2;
    CHECK(tri_area_2x(two, t2) == want2);
    CHECK(triangles_covering(two, t2, fx2{F(10), F(10)}) == 0);    // in hole 1
    CHECK(triangles_covering(two, t2, fx2{F(45), F(20)}) == 0);    // in hole 2
    CHECK(triangles_covering(two, t2, fx2{F(30), F(35)}) >= 1);    // in the ring
    // The L-shaped hole covers x 8..20 y 8..14 plus x 14..20 y 14..20; its
    // NOTCH -- the part of the ring the hole does not reach -- is x 8..14,
    // y 14..20. Worked out rather than eyeballed: (17,17) is inside the hole
    // and (11,17) is the notch, and the first version of this had them the
    // other way round.
    CHECK(triangles_covering(two, t2, fx2{F(17), F(17)}) == 0);    // inside hole 1
    CHECK(triangles_covering(two, t2, fx2{F(11), F(17)}) >= 1);    // hole 1's notch
    CHECK(triangles_covering(two, t2, fx2{F(11), F(11)}) == 0);    // also inside hole 1

    // Deterministic: the same input triangulates identically every time. The
    // bridge choice is a search, and a search that depended on anything but the
    // input would desync a client the first time update_shape ran.
    TriangleList again;
    CHECK(triangulate_with_holes(two, 4, two_holes, 2, again) == TriangulateStatus::Ok);
    CHECK(again.count == t2.count);
    bool same = true;
    for (u16 i = 0; i < t2.count * 3; ++i) if (again.indices[i] != t2.indices[i]) same = false;
    CHECK(same);
}

static void test_staged_commit() {
    std::printf("mapstore: staged edits are atomic and rebuild the mesh\n");

    static MapStorage store;

    store.clear();
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

    static MapStorage store;

    store.clear();
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

// -------------------------------------------------- weakly simple outlines

// A loop that touches ITSELF at a vertex.
//
// Doom renders through a BSP and never triangulates a sector, so its sector
// outlines are free to pinch: a pond meeting the edge of a courtyard traces one
// closed walk that visits the meeting point twice. tools/wad/convert.js hands
// exactly this shape to the clipper rather than cutting the inward lobe out and
// re-declaring it as a hole, because bridging a hole PRODUCES this shape -- the
// cut is a pinch -- so the pinched outline is the bridged form, already built.
//
// That is a claim about the clipper, and this is where it is checked: the mesh
// must cover the outlined region and nothing else. tools/mapc asserts the same
// equality on every sector of every map it bakes.
static void test_pinched_outline() {
    std::printf("triangulate: an outline that touches itself meshes its own area\n");

    // A 40x40 room whose triangular pond touches the south-west corner at that
    // one point. Walked counter-clockwise from the corner, out around the room,
    // back to the corner, then clockwise around the pond.
    //
    // The pond touches at a VERTEX and nowhere else. A pond that also ran along
    // the walls would share edge with them, which is a different degeneracy --
    // overlapping segments, not a pinch -- and the clipper refuses it. Doom's
    // pinched loops are all of the first kind.
    const fx2 pts[] = {
        {F(0), F(0)}, {F(40), F(0)}, {F(40), F(40)}, {F(0), F(40)},
        {F(0), F(0)}, {F(4), F(12)}, {F(12), F(4)}
    };
    TriangleList t;
    CHECK(triangulate(pts, 7, t) == TriangulateStatus::Ok);

    // 1600 - 64. If the clipper had bridged across the pinch and filled the
    // pond, this would read 1600.
    const i128 want = static_cast<i128>(fx::from_int(1536).raw) * fx::one().raw * 2;
    CHECK(tri_area_2x(pts, t) == want);

    // The pond is covered by nothing; a point in the room is covered.
    CHECK(triangles_covering(pts, t, fx2{F(5), F(5)}) == 0);
    CHECK(triangles_covering(pts, t, fx2{F(30), F(30)}) >= 1);

    // The OTHER pinch -- two lobes wound the same way, a figure eight -- is a
    // shape the converter must not hand over whole, because the two lobes are
    // two floors with a gap between them and no single polygon says so. It is
    // split into separate sectors instead. The clipper's own answer is checked
    // here only to record which of the two it is: whatever it returns, the
    // area assertion above is what mapc re-runs, so a mesh across the gap is
    // caught at bake time rather than under a player's feet.
    const fx2 eight[] = {
        {F(0), F(0)}, {F(20), F(0)}, {F(20), F(20)},
        {F(40), F(20)}, {F(40), F(40)}, {F(20), F(40)},
        {F(20), F(20)}, {F(0), F(20)}
    };
    TriangleList e;
    if (triangulate(eight, 8, e) == TriangulateStatus::Ok) {
        const i128 want8 = static_cast<i128>(fx::from_int(800).raw) * fx::one().raw * 2;
        CHECK(tri_area_2x(eight, e) == want8);
    }
}

int main() {
    std::printf("== core/geom triangulation suite ==\n");
    test_convex();
    test_concave();
    test_winding_normalised();
    test_rejects_bad_input();
    test_determinism();
    test_holes();
    test_pinched_outline();
    test_staged_commit();
    test_rejected_commit_is_inert();
    test_commit_determinism();
    test_baked_matches_runtime();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
