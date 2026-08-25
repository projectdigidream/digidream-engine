//
// Rails and wires (§3.4).
//
// The claim that matters here is the one in the design's own wording: waypoints
// are sector-relative "so rails on moving platforms work with no special
// casing". So the headline test is not that a rail can be measured -- it is
// that a rail bolted to a lift moves with it while an identical rail bolted to
// the ground does not, with nothing in the rail code aware that either
// happened.
//
#include <cstdio>

#include "dnp/geom/mapfile.hpp"
#include "dnp/geom/rail.hpp"
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

// Two rooms; sector 1 is a lift. Rails are added per test.
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

// ------------------------------------------------------------------- geometry

static void test_length_and_position_along() {
    std::printf("rail: length, and the position a given distance along it\n");

    static Stage st;
    const u32 r = st.store.add_rail(1, 0);
    st.store.add_waypoint(r, fx3{F(0), F(0), F(2)}, NO_SECTOR);
    st.store.add_waypoint(r, fx3{F(8), F(0), F(2)}, NO_SECTOR);
    st.store.add_waypoint(r, fx3{F(8), F(6), F(2)}, NO_SECTOR);
    st.refresh();

    CHECK(rail_segment_count(st.view, r) == 2);
    CHECK(rail_length(st.view, r) == F(14));

    // Both ends are EXACT. A rider parked at a waypoint has to be at it rather
    // than an ulp short, or the next segment's arithmetic starts from an error
    // that the segment after that inherits.
    CHECK(rail_point_at(st.view, r, fx::zero()).pos == (fx3{F(0), F(0), F(2)}));
    CHECK(rail_point_at(st.view, r, F(8)).pos == (fx3{F(8), F(0), F(2)}));
    CHECK(rail_point_at(st.view, r, F(14)).pos == (fx3{F(8), F(6), F(2)}));

    const RailPoint mid = rail_point_at(st.view, r, F(4));
    CHECK(mid.pos == (fx3{F(4), F(0), F(2)}));
    CHECK(mid.segment == 0);
    CHECK(!mid.clamped);

    const RailPoint second = rail_point_at(st.view, r, F(11));
    CHECK(second.pos == (fx3{F(8), F(3), F(2)}));
    CHECK(second.segment == 1);

    // Running off the end CLAMPS and says so. Saying so is the point: the end
    // of a rail is where a controller hands back to standard physics, and a
    // silent clamp makes "arrived" indistinguishable from "still going".
    const RailPoint past = rail_point_at(st.view, r, F(100));
    CHECK(past.pos == (fx3{F(8), F(6), F(2)}));
    CHECK(past.clamped);
    const RailPoint before = rail_point_at(st.view, r, F(-5));
    CHECK(before.pos == (fx3{F(0), F(0), F(2)}));
    CHECK(before.clamped);
    std::printf("  three waypoints, length %.1f, both ends exact\n", D(rail_length(st.view, r)));
}

static void test_a_loop_wraps_instead_of_clamping() {
    std::printf("rail: a looping rail wraps, and never reports a clamp\n");

    static Stage st;
    const u32 r = st.store.add_rail(1, rail_flag::LOOP);
    st.store.add_waypoint(r, fx3{F(0), F(0), F(2)}, NO_SECTOR);
    st.store.add_waypoint(r, fx3{F(10), F(0), F(2)}, NO_SECTOR);
    st.store.add_waypoint(r, fx3{F(10), F(10), F(2)}, NO_SECTOR);
    st.store.add_waypoint(r, fx3{F(0), F(10), F(2)}, NO_SECTOR);
    st.refresh();

    // Four waypoints, four segments: the closure counts.
    CHECK(rail_segment_count(st.view, r) == 4);
    CHECK(rail_length(st.view, r) == F(40));

    const fx3 start{F(0), F(0), F(2)};
    CHECK(rail_point_at(st.view, r, fx::zero()).pos == start);
    // Exactly one lap lands exactly back at the start -- not near it. A loop
    // that drifted an ulp per lap is a rail that has moved by the end of a
    // round.
    CHECK(rail_point_at(st.view, r, F(40)).pos == start);
    CHECK(rail_point_at(st.view, r, F(120)).pos == start);
    CHECK(rail_point_at(st.view, r, F(-40)).pos == start);
    CHECK(rail_point_at(st.view, r, F(45)).pos == rail_point_at(st.view, r, F(5)).pos);
    CHECK(rail_point_at(st.view, r, F(-5)).pos == rail_point_at(st.view, r, F(35)).pos);
    CHECK(!rail_point_at(st.view, r, F(1000)).clamped);
    std::printf("  four segments, length %.1f, a lap returns exactly to the start\n",
                D(rail_length(st.view, r)));
}

static void test_nearest_point_on_a_rail() {
    std::printf("rail: the nearest point, which is how an entity attaches\n");

    static Stage st;
    const u32 r = st.store.add_rail(1, 0);
    st.store.add_waypoint(r, fx3{F(0), F(0), F(0)}, NO_SECTOR);
    st.store.add_waypoint(r, fx3{F(10), F(0), F(0)}, NO_SECTOR);
    st.store.add_waypoint(r, fx3{F(10), F(10), F(0)}, NO_SECTOR);
    st.refresh();

    // Straight above the middle of the first segment.
    RailNearest n = rail_nearest(st.view, r, fx3{F(5), F(0), F(3)});
    CHECK(n.pos == (fx3{F(5), F(0), F(0)}));
    CHECK(n.distance_along == F(5));
    CHECK(n.segment == 0);
    CHECK(n.dist_sq == len_sq_raw(fx3{fx::zero(), fx::zero(), F(3)}));

    // Off the far end: clamps to the endpoint rather than running past it.
    n = rail_nearest(st.view, r, fx3{F(10), F(50), F(0)});
    CHECK(n.pos == (fx3{F(10), F(10), F(0)}));
    CHECK(n.distance_along == F(20));

    // Off the near end.
    n = rail_nearest(st.view, r, fx3{F(-7), F(0), F(0)});
    CHECK(n.pos == (fx3{F(0), F(0), F(0)}));
    CHECK(n.distance_along == fx::zero());

    // Inside the corner, equidistant from both segments. Resolved to the
    // EARLIER segment, so an entity attaching at a corner does not flip between
    // two answers depending on which the loop happened to reach last.
    n = rail_nearest(st.view, r, fx3{F(7), F(3), F(0)});
    CHECK(n.segment == 0);
    std::printf("  attaches at the right point, clamps at both ends\n");
}

// --------------------------------------------------------- the headline claim

static void test_a_rail_rides_its_sector() {
    std::printf("rail: a rail bolted to a lift moves with it; an identical one on the ground does not\n");

    static Stage st;

    // Two rails with the SAME authored geometry, differing only in what they
    // are anchored to. Anything that made both move, or neither, passes a test
    // with one rail in it.
    const u32 riding = st.store.add_rail(1, 0);
    st.store.add_waypoint(riding, fx3{F(24), F(4), F(2)}, 1);
    st.store.add_waypoint(riding, fx3{F(36), F(4), F(2)}, 1);

    const u32 fixed = st.store.add_rail(2, 0);
    st.store.add_waypoint(fixed, fx3{F(4), F(4), F(2)}, 0);
    st.store.add_waypoint(fixed, fx3{F(16), F(4), F(2)}, 0);
    st.refresh();

    static World w;
    w.reset();
    w.set_map(&st.view);
    w.set_map_storage(&st.store);

    w.motions().add(st.store, 1);
    w.motions().start_parametric(1, fx3::zero(), fx3{fx::zero(), fx::zero(), F(6)},
                                 0, 120, LoopMode::PingPong);

    const fx3 riding_base = st.view.waypoint(st.view.rail(riding).first_waypoint).base;
    const fx3 fixed_base = st.view.waypoint(st.view.rail(fixed).first_waypoint).base;

    fx max_rise = fx::zero();
    bool fixed_ever_moved = false;
    bool tracked = true;

    for (u32 t = 0; t < 240; ++t) {
        w.step();
        st.refresh();

        const fx3 rp = st.view.waypoint(st.view.rail(riding).first_waypoint).pos;
        const fx3 fp = st.view.waypoint(st.view.rail(fixed).first_waypoint).pos;

        // The rail is exactly the sector's offset away from where it was
        // authored -- base + offset, never an accumulated increment.
        const fx3 offset = w.motions().at(0).offset;
        if (!(rp == riding_base + offset)) tracked = false;
        if (!(fp == fixed_base)) fixed_ever_moved = true;

        const fx rise = rp.z - riding_base.z;
        if (rise > max_rise) max_rise = rise;
    }

    CHECK(tracked);
    CHECK(!fixed_ever_moved);
    CHECK(max_rise == F(6));
    std::printf("  the riding rail rose %.1f units, the fixed one never moved\n", D(max_rise));

    // And the queries see it without knowing anything about sectors: the moving
    // rail's midpoint is above where it was authored.
    const RailPoint mid = rail_point_at(st.view, riding, F(6));
    CHECK(mid.pos.x == F(30));
    CHECK(mid.pos.z == riding_base.z + w.motions().at(0).offset.z);

    // A full ping-pong returns it EXACTLY home. A rail that drifted an ulp per
    // cycle is a rail that is somewhere else by the end of a match.
    //
    // One more step first: the geometry a step writes is for the tick it is
    // ENTERING, so after 240 steps the world reads tick 240 while the rails
    // still show 239 -- one frame short of the cycle boundary.
    CHECK(w.tick() == 240);
    w.step();
    st.refresh();
    CHECK(w.motions().at(0).offset == fx3::zero());
    CHECK(st.view.waypoint(st.view.rail(riding).first_waypoint).pos == riding_base);
}

// ------------------------------------------------------------------ junctions

// What a waypoint anchored to a one-surface mover follows.
//
// Written because the alternative implementation -- accumulating each tick's
// FLOOR displacement instead of reading the sector's offset -- passes every
// other test in this file. The two answers only diverge for a sector that moves
// one surface, and a door is exactly that.
static void test_a_rail_on_a_moving_ceiling_follows_the_ceiling() {
    std::printf("rail: a rail anchored to a door follows the surface the door moves\n");

    static Stage st;
    const u32 hung = st.store.add_rail(1, rail_flag::WIRE);
    st.store.add_waypoint(hung, fx3{F(24), F(4), F(30)}, 1);
    st.store.add_waypoint(hung, fx3{F(36), F(4), F(30)}, 1);
    st.refresh();

    static World w;
    w.reset();
    w.set_map(&st.view);
    w.set_map_storage(&st.store);

    // A ceiling coming down. Its FLOOR never moves -- so a waypoint carried by
    // the floor's displacement would sit still while the ceiling it hangs from
    // descended past it.
    w.motions().add(st.store, 1);
    w.motions().start_parametric(1, fx3::zero(), fx3{fx::zero(), fx::zero(), F(-12)},
                                 0, 120, LoopMode::Once, MotionTarget::Ceiling);

    const fx3 base = st.view.waypoint(st.view.rail(hung).first_waypoint).base;
    for (u32 t = 0; t < 200; ++t) w.step();
    st.refresh();

    CHECK(w.motions().at(0).floor_delta().z == fx::zero());
    CHECK(st.store.sector_mut(1).floor_z == F(0));
    CHECK(st.store.sector_mut(1).ceil_z == F(28));
    // The wire came down with the ceiling it is bolted to.
    const fx3 dropped = base + fx3{fx::zero(), fx::zero(), F(-12)};
    CHECK(st.view.waypoint(st.view.rail(hung).first_waypoint).pos == dropped);
    std::printf("  ceiling fell 12.0 and took the wire with it, floor unmoved\n");
}

static void test_junctions_are_reported_never_chosen() {
    std::printf("rail: a fork reports every branch and takes none (§7.4)\n");

    static Stage st;
    const u32 trunk = st.store.add_rail(1, 0);
    st.store.add_waypoint(trunk, fx3{F(0), F(0), F(2)}, NO_SECTOR);
    st.store.add_waypoint(trunk, fx3{F(10), F(0), F(2)}, NO_SECTOR);
    const u32 left = st.store.add_rail(2, 0);
    st.store.add_waypoint(left, fx3{F(10), F(0), F(2)}, NO_SECTOR);
    st.store.add_waypoint(left, fx3{F(10), F(10), F(2)}, NO_SECTOR);
    const u32 right = st.store.add_rail(3, 0);
    st.store.add_waypoint(right, fx3{F(10), F(0), F(2)}, NO_SECTOR);
    st.store.add_waypoint(right, fx3{F(10), F(-10), F(2)}, NO_SECTOR);

    CHECK(st.store.add_rail_link(trunk, 1, left, 0));
    CHECK(st.store.add_rail_link(trunk, 1, right, 0));
    // A junction naming a waypoint that does not exist is refused here, not
    // discovered at the first fork.
    CHECK(!st.store.add_rail_link(trunk, 9, left, 0));
    CHECK(!st.store.add_rail_link(trunk, 1, 99, 0));
    st.refresh();

    CHECK(rail_link_count_at(st.view, trunk, 0) == 0);
    CHECK(rail_link_count_at(st.view, trunk, 1) == 2);

    const i32 a = rail_link_at(st.view, trunk, 1, 0);
    const i32 b = rail_link_at(st.view, trunk, 1, 1);
    CHECK(a >= 0 && b >= 0 && a != b);
    CHECK(st.view.rail_link(static_cast<u32>(a)).to_rail == left);
    CHECK(st.view.rail_link(static_cast<u32>(b)).to_rail == right);
    // Enumerated, in a fixed order, and nothing here decides. Picking a branch
    // is Lua's job; an engine that quietly took the first one would make the
    // Lua callback undetectably dead code.
    CHECK(rail_link_at(st.view, trunk, 1, 2) == -1);
    std::printf("  a two-way fork enumerates both branches in a fixed order\n");
}

// -------------------------------------------------------------- the file form

static void test_rails_survive_the_baked_format() {
    std::printf("rail: rails, anchors and junctions round-trip through .dnpm\n");

    static Stage st;
    const u32 r0 = st.store.add_rail(7, rail_flag::WIRE | rail_flag::LOOP);
    st.store.add_waypoint(r0, fx3{F(2), F(2), F(3)}, 0);
    st.store.add_waypoint(r0, fx3{F(30), F(2), F(3)}, 1);
    st.store.add_waypoint(r0, fx3{F(30), F(9), F(3)}, NO_SECTOR);
    const u32 r1 = st.store.add_rail(8, 0);
    st.store.add_waypoint(r1, fx3{F(30), F(9), F(3)}, NO_SECTOR);
    st.store.add_waypoint(r1, fx3{F(2), F(9), F(3)}, 0);
    st.store.add_rail_link(r0, 2, r1, 0);
    st.refresh();

    static u8 buf[1 << 16];
    ByteWriter w(buf, sizeof(buf));
    CHECK(save_map(st.store, w));

    static MapStorage loaded;
    ByteReader rd(buf, w.size());
    const MapLoadStatus status = load_map(rd, loaded);
    CHECK(status == MapLoadStatus::Ok);
    if (status != MapLoadStatus::Ok) {
        std::printf("  load failed: %s\n", map_load_status_text(status));
        return;
    }

    const MapData m = loaded.view();
    CHECK(m.rail_count == 2);
    CHECK(m.waypoint_count == 5);
    CHECK(m.rail_link_count == 1);
    CHECK(m.rail(0).flags == (rail_flag::WIRE | rail_flag::LOOP));
    CHECK(m.rail(0).tag == 7);
    CHECK(m.rail(1).first_waypoint == 3);
    // Anchors survive, including the deliberate NO_SECTOR one. An anchor
    // silently defaulting to 0 would make every world-fixed rail ride sector 0.
    CHECK(m.waypoint(0).anchor == 0);
    CHECK(m.waypoint(1).anchor == 1);
    CHECK(m.waypoint(2).anchor == NO_SECTOR);
    CHECK(m.waypoint(1).base == (fx3{F(30), F(2), F(3)}));
    // A freshly loaded waypoint is already live, so a map with no moving
    // sectors needs no per-tick pass before anything can read a rail.
    CHECK(m.waypoint(1).pos == m.waypoint(1).base);
    CHECK(m.rail_link(0).rail == 0 && m.rail_link(0).waypoint == 2 && m.rail_link(0).to_rail == 1);

    // The hash covers rails, so a client whose rails differ from the server's is
    // detectable rather than mysterious.
    CHECK(loaded.hash() == st.store.hash());
    std::printf("  %u rails, %u waypoints, %u junctions, hash %016llX\n",
                m.rail_count, m.waypoint_count, m.rail_link_count,
                static_cast<unsigned long long>(loaded.hash()));
}

static void test_a_malformed_rail_section_is_refused() {
    std::printf("rail: a malformed rail section is refused, never partly loaded\n");

    static Stage st;
    const u32 r = st.store.add_rail(1, 0);
    st.store.add_waypoint(r, fx3{F(2), F(2), F(3)}, 0);
    st.store.add_waypoint(r, fx3{F(9), F(2), F(3)}, 0);
    st.refresh();

    static u8 good[1 << 16];
    ByteWriter w(good, sizeof(good));
    CHECK(save_map(st.store, w));
    const size_t n = w.size();

    // Every single-byte corruption either loads to an identical map or is
    // rejected. What must never happen is a map that loads and is DIFFERENT --
    // that is the shape of bug where a client and server quietly run different
    // geometry.
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

int main() {
    test_length_and_position_along();
    test_a_loop_wraps_instead_of_clamping();
    test_nearest_point_on_a_rail();
    test_a_rail_rides_its_sector();
    test_a_rail_on_a_moving_ceiling_follows_the_ceiling();
    test_junctions_are_reported_never_chosen();
    test_rails_survive_the_baked_format();
    test_a_malformed_rail_section_is_refused();

    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
