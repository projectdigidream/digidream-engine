//
// Camera paths (§4.1).
//
// A camera path is the one thing in the map file that NOTHING in core or the
// server ever reads. The client's camera reads it, and only the client's.
//
// That makes this suite's job unusual. There is no behaviour to pin -- no
// per-tick rule, no query, no golden trace -- so what is left is the two
// properties the client depends on and cannot check for itself:
//
//   1. the file round-trips, and a corrupted one is refused rather than
//      silently loaded as a different path. The hash is the ENTIRE agreement
//      between the client and the server here: if a path's points differed
//      between them, no other part of the system would ever notice.
//   2. a path anchored to a moving sector rides it, drift-free. This is the
//      only work the simulation does for paths, and it is the whole reason they
//      are map data instead of script data.
//
// And one property about the separation itself: that adding paths did not
// loosen the contiguity rule protecting rails, which is why they have their own
// arrays in the first place.
//
#include <cstdio>

#include "dnp/geom/mapfile.hpp"
#include "dnp/geom/mapstore.hpp"
#include "dnp/sim/world.hpp"

using namespace dnp;
using namespace dnp::geom;
using namespace dnp::sim;

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
static double D(fx v) { return static_cast<double>(v.raw) / 4294967296.0; }

// Two rooms; sector 1 is a lift. Paths are added per test.
struct Stage {
    MapStorage store;
    MapData    view{};

    Stage() {
        const fx2 a[] = {{F(0), F(0)}, {F(20), F(0)}, {F(20), F(20)}, {F(0), F(20)}};
        const fx2 b[] = {{F(20), F(0)}, {F(40), F(0)}, {F(40), F(20)}, {F(20), F(20)}};
        store.add_sector(a, 4, F(0), F(40), 1);
        store.add_sector(b, 4, F(0), F(40), 2);
        store.add_side(0, 0, 1, NO_SECTOR, 0);
        store.add_side(0, 1, 2, 1, 0);
        store.add_side(0, 2, 3, NO_SECTOR, 0);
        store.add_side(0, 3, 0, NO_SECTOR, 0);
        store.add_side(1, 0, 1, NO_SECTOR, 0);
        store.add_side(1, 1, 2, NO_SECTOR, 0);
        store.add_side(1, 2, 3, NO_SECTOR, 0);
        store.add_side(1, 3, 0, 0, 0);
        refresh();
    }
    void refresh() { view = store.view(); }
};

// ------------------------------------------------------------------- building

static void test_a_path_is_built_and_read_back() {
    std::printf("path: points, flags and the baked curve length survive the store\n");

    static Stage st;
    const u32 p = st.store.add_path(7, path_flag::LOOP, F(42));
    CHECK(p == 0);
    CHECK(st.store.add_path_point(p, fx3{F(2), F(2), F(1)}, 0));
    CHECK(st.store.add_path_point(p, fx3{F(18), F(2), F(1)}, 0));
    CHECK(st.store.add_path_point(p, fx3{F(18), F(8), F(1)}, 0));
    st.refresh();

    CHECK(st.view.path_count == 1);
    CHECK(st.view.path_point_count == 3);
    const Path& got = st.view.path(0);
    CHECK(got.tag == 7);
    CHECK((got.flags & path_flag::LOOP) != 0);
    CHECK(got.first_point == 0 && got.point_count == 3);
    CHECK(got.curve_length == F(42));
    CHECK(st.view.path_point(1).base.x == F(18));
    // Live from the moment it exists, so a map with no movers needs no per-tick
    // pass and a reader never meets an unresolved point.
    CHECK(st.view.path_point(1).pos == st.view.path_point(1).base);

    // Only the most recently started path may grow -- the same rule as
    // add_waypoint, and for the same reason: a non-contiguous range cannot be
    // rebuilt by the loader.
    const u32 q = st.store.add_path(8, 0, F(3));
    CHECK(q == 1);
    CHECK(!st.store.add_path_point(p, fx3{F(0), F(0), F(0)}, NO_SECTOR));
    CHECK(st.store.add_path_point(q, fx3{F(1), F(1), F(1)}, NO_SECTOR));
}

// ------------------------------------------------------------------- movement

static void test_a_path_rides_its_sector() {
    std::printf("path: a path bolted to a lift moves with it; an identical one on the ground does not\n");

    static Stage st;

    // Two paths with the SAME authored shape, differing only in their anchor.
    // Anything that moved both, or neither, passes a test with one path in it.
    const u32 riding = st.store.add_path(1, 0, F(12));
    st.store.add_path_point(riding, fx3{F(24), F(4), F(2)}, 1);
    st.store.add_path_point(riding, fx3{F(36), F(4), F(2)}, 1);

    const u32 fixed = st.store.add_path(2, 0, F(12));
    st.store.add_path_point(fixed, fx3{F(4), F(4), F(2)}, 0);
    st.store.add_path_point(fixed, fx3{F(16), F(4), F(2)}, 0);
    st.refresh();

    static World w;
    w.reset();
    w.set_map(&st.view);
    w.set_map_storage(&st.store);

    w.motions().add(st.store, 1);
    w.motions().start_parametric(1, fx3::zero(), fx3{fx::zero(), fx::zero(), F(6)},
                                 0, 120, LoopMode::PingPong);

    const fx3 riding_base = st.view.path_point(st.view.path(riding).first_point).base;
    const fx3 fixed_base = st.view.path_point(st.view.path(fixed).first_point).base;

    fx max_rise = fx::zero();
    bool fixed_ever_moved = false;
    bool tracked = true;

    for (u32 t = 0; t < 240; ++t) {
        w.step();
        st.refresh();

        const fx3 rp = st.view.path_point(st.view.path(riding).first_point).pos;
        const fx3 fp = st.view.path_point(st.view.path(fixed).first_point).pos;

        // base + offset, never an accumulated increment -- which is what makes
        // it a pure function of the tick, and what lets a predicting client
        // evaluate a path for a tick the server has not reached.
        const fx3 offset = w.motions().at(0).offset;
        if (!(rp == riding_base + offset)) tracked = false;
        if (!(fp == fixed_base)) fixed_ever_moved = true;

        const fx rise = rp.z - riding_base.z;
        if (rise > max_rise) max_rise = rise;
    }

    CHECK(tracked);
    CHECK(!fixed_ever_moved);
    CHECK(max_rise == F(6));
    std::printf("  the riding path rose %.1f units, the fixed one never moved\n", D(max_rise));

    // A full ping-pong returns it EXACTLY home. A path that drifted an ulp per
    // cycle would put a scripted camera somewhere else by the end of a match.
    //
    // One more step first: the geometry a step writes is for the tick it is
    // ENTERING, so after 240 steps the world reads tick 240 while the points
    // still show 239 -- one frame short of the cycle boundary.
    CHECK(w.tick() == 240);
    w.step();
    st.refresh();
    CHECK(w.motions().at(0).offset == fx3::zero());
    CHECK(st.view.path_point(st.view.path(riding).first_point).pos == riding_base);
}

// ----------------------------------------------------------------- the format

static void test_paths_survive_the_baked_format() {
    std::printf("path: a path's points, flags and curve length survive the file\n");

    static Stage st;
    const u32 a = st.store.add_path(3, path_flag::LOOP, fx::from_frac(2240577, 100000));
    st.store.add_path_point(a, fx3{F(2), F(2), F(1)}, 0);
    st.store.add_path_point(a, fx3{F(18), F(2), F(1)}, 0);
    st.store.add_path_point(a, fx3{F(18), F(8), F(1)}, 1);
    const u32 b = st.store.add_path(4, 0, F(16));
    st.store.add_path_point(b, fx3{F(2), F(12), F(2)}, NO_SECTOR);
    st.store.add_path_point(b, fx3{F(18), F(12), F(2)}, NO_SECTOR);
    st.refresh();

    static u8 buf[1 << 16];
    ByteWriter w(buf, sizeof(buf));
    CHECK(save_map(st.store, w));

    static MapStorage out;
    ByteReader r(buf, w.size());
    CHECK(load_map(r, out) == MapLoadStatus::Ok);
    const MapData m = out.view();

    CHECK(m.path_count == 2);
    CHECK(m.path_point_count == 5);
    CHECK(m.path(0).tag == 3 && (m.path(0).flags & path_flag::LOOP) != 0);
    CHECK(m.path(1).tag == 4 && m.path(1).flags == 0);
    CHECK(m.path(0).point_count == 3 && m.path(1).point_count == 2);
    // The curve length is carried at full fx precision, not rounded to a unit:
    // it is divided into a speed to time a cutscene, and the error compounds.
    CHECK(m.path(0).curve_length == st.view.path(0).curve_length);
    CHECK(m.path(1).curve_length == F(16));
    CHECK(m.path_point(2).anchor == 1);
    CHECK(m.path_point(3).anchor == NO_SECTOR);
    CHECK(m.path_point(4).base.x == F(18));
    CHECK(out.hash() == st.store.hash());

    // Saving twice gives the same bytes. Only `base` is written, never the live
    // `pos`, so a map saved after a lift has moved is the same file as one saved
    // before -- which is what keeps the hash a fact about the FILE.
    static u8 again[1 << 16];
    ByteWriter w2(again, sizeof(again));
    CHECK(save_map(out, w2));
    CHECK(w2.size() == w.size());
    bool identical = true;
    for (size_t i = 0; i < w.size(); ++i) if (again[i] != buf[i]) identical = false;
    CHECK(identical);
}

static void test_the_hash_notices_a_path() {
    std::printf("path: the map hash covers paths -- the client is their only reader\n");

    // Paths are the one thing in the file that only the CLIENT reads. If its
    // points differed from the server's, nothing else in the system would ever
    // notice -- so the hash is not a backstop here, it is the entire agreement.
    static Stage bare;
    const u64 without = bare.store.hash();

    static Stage with;
    const u32 p = with.store.add_path(1, 0, F(10));
    with.store.add_path_point(p, fx3{F(2), F(2), F(1)}, 0);
    with.store.add_path_point(p, fx3{F(18), F(2), F(1)}, 0);
    CHECK(with.store.hash() != without);

    // A point moved by one ulp is a different map.
    static Stage nudged;
    const u32 q = nudged.store.add_path(1, 0, F(10));
    nudged.store.add_path_point(q, fx3{F(2), F(2), F(1)}, 0);
    nudged.store.add_path_point(q, fx3{fx::from_raw(F(18).raw + 1), F(2), F(1)}, 0);
    CHECK(nudged.store.hash() != with.store.hash());

    // So is one whose CURVE LENGTH differs, with identical points. That field
    // sets how long a cutscene runs, and it is written to the file, so a build
    // that did not hash it would leave a bit an attacker could flip in silence.
    static Stage relabelled;
    const u32 s = relabelled.store.add_path(1, 0, F(11));
    relabelled.store.add_path_point(s, fx3{F(2), F(2), F(1)}, 0);
    relabelled.store.add_path_point(s, fx3{F(18), F(2), F(1)}, 0);
    CHECK(relabelled.store.hash() != with.store.hash());

    // CONTROL: the same path built twice hashes the same, so the checks above
    // are noticing the difference and not merely noticing that hashing happened.
    static Stage twin;
    const u32 t = twin.store.add_path(1, 0, F(10));
    twin.store.add_path_point(t, fx3{F(2), F(2), F(1)}, 0);
    twin.store.add_path_point(t, fx3{F(18), F(2), F(1)}, 0);
    CHECK(twin.store.hash() == with.store.hash());
}

static void test_a_malformed_path_section_is_refused() {
    std::printf("path: a malformed path section is refused, never partly loaded\n");

    static Stage st;
    const u32 p = st.store.add_path(1, 0, F(22));
    st.store.add_path_point(p, fx3{F(2), F(2), F(3)}, 0);
    st.store.add_path_point(p, fx3{F(9), F(2), F(3)}, 0);
    st.refresh();

    static u8 good[1 << 16];
    ByteWriter w(good, sizeof(good));
    CHECK(save_map(st.store, w));
    const size_t n = w.size();

    // Every single-byte corruption either loads to an identical map or is
    // rejected. What must never happen is a map that loads and is DIFFERENT.
    // For a camera path that is the whole ballgame: it is the only geometry in
    // the file whose two readers have no other way to disagree loudly.
    static MapStorage out;
    u32 rejected = 0;
    u32 accepted = 0;
    u32 wrong = 0;
    static u8 scratch[1 << 16];
    for (size_t i = 0; i < n; ++i) {
        for (u8 bit = 0; bit < 8; ++bit) {
            for (size_t k = 0; k < n; ++k) scratch[k] = good[k];
            scratch[i] ^= static_cast<u8>(1u << bit);
            ByteReader rd(scratch, n);
            if (load_map(rd, out) != MapLoadStatus::Ok) { ++rejected; continue; }
            ++accepted;
            if (out.hash() != st.store.hash()) ++wrong;
        }
    }
    CHECK(wrong == 0);
    CHECK(rejected > 0);
    std::printf("  %zu bytes, %u flips: %u rejected, %u accepted, %u silently different\n",
                n, static_cast<unsigned>(n * 8), rejected, accepted, wrong);
}

// -------------------------------------------------------------- the separation

static void test_paths_and_rails_do_not_share_anything() {
    std::printf("path: paths did not loosen the contiguity rule that protects rails\n");

    // The reason paths got their own arrays. A path taking a slice of the
    // waypoint array would have meant relaxing the rule at mapfile.cpp that
    // rails cover it exactly and contiguously -- putting the cosmetic half
    // inside the invariant that protects the simulated one.
    //
    // Each of these three maps must load, and the two counts must stay
    // independent through the file.
    {
        // Paths and no rails.
        static Stage st;
        const u32 p = st.store.add_path(1, 0, F(9));
        st.store.add_path_point(p, fx3{F(2), F(2), F(1)}, 0);
        st.store.add_path_point(p, fx3{F(11), F(2), F(1)}, 0);
        static u8 buf[1 << 16];
        ByteWriter w(buf, sizeof(buf));
        CHECK(save_map(st.store, w));
        static MapStorage out;
        ByteReader r(buf, w.size());
        CHECK(load_map(r, out) == MapLoadStatus::Ok);
        CHECK(out.path_count() == 1 && out.waypoint_count() == 0 && out.rail_count() == 0);
    }
    {
        // Rails and no paths -- the shape every map in the repo had before this.
        static Stage st;
        const u32 rr = st.store.add_rail(1, 0);
        st.store.add_waypoint(rr, fx3{F(2), F(2), F(1)}, 0);
        st.store.add_waypoint(rr, fx3{F(11), F(2), F(1)}, 0);
        static u8 buf[1 << 16];
        ByteWriter w(buf, sizeof(buf));
        CHECK(save_map(st.store, w));
        static MapStorage out;
        ByteReader r(buf, w.size());
        CHECK(load_map(r, out) == MapLoadStatus::Ok);
        CHECK(out.rail_count() == 1 && out.waypoint_count() == 2);
        CHECK(out.path_count() == 0 && out.path_point_count() == 0);
    }
    {
        // Both, with DIFFERENT counts, so a reader that confused one range for
        // the other could not come out even.
        static Stage st;
        const u32 rr = st.store.add_rail(1, 0);
        st.store.add_waypoint(rr, fx3{F(2), F(2), F(1)}, 0);
        st.store.add_waypoint(rr, fx3{F(11), F(2), F(1)}, 0);
        const u32 p = st.store.add_path(5, path_flag::LOOP, F(30));
        st.store.add_path_point(p, fx3{F(3), F(3), F(6)}, 0);
        st.store.add_path_point(p, fx3{F(17), F(3), F(6)}, 0);
        st.store.add_path_point(p, fx3{F(17), F(17), F(6)}, 0);
        static u8 buf[1 << 16];
        ByteWriter w(buf, sizeof(buf));
        CHECK(save_map(st.store, w));
        static MapStorage out;
        ByteReader r(buf, w.size());
        CHECK(load_map(r, out) == MapLoadStatus::Ok);
        CHECK(out.rail_count() == 1 && out.waypoint_count() == 2);
        CHECK(out.path_count() == 1 && out.path_point_count() == 3);
        const MapData m = out.view();
        // And the two kinds of point are genuinely separate storage: the rail's
        // first waypoint and the path's first point are both index 0 of their
        // own array, and they are different places.
        CHECK(m.waypoint(0).base.z == F(1));
        CHECK(m.path_point(0).base.z == F(6));
        CHECK((m.path(0).flags & path_flag::LOOP) != 0);
        CHECK(m.rail(0).flags == 0);
    }
}

int main() {
    test_a_path_is_built_and_read_back();
    test_a_path_rides_its_sector();
    test_paths_survive_the_baked_format();
    test_the_hash_notices_a_path();
    test_a_malformed_path_section_is_refused();
    test_paths_and_rails_do_not_share_anything();

    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
