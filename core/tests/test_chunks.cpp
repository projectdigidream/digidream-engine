//
// Several maps in one store, added and removed while the world runs
// (geom::MapStorage::add_chunk / remove_chunk).
//
// The question each part answers is whether a body can walk from one map into
// the next the way it walks between two rooms -- with no hand-over, no second
// simulation and no special case -- and whether that survives the maps coming
// and going underneath it. Every claim has a control that fails without the
// join, because "the body got there" is also what a map with no gap in it
// would produce.
//
// Two chunks, each a room and a corridor, like maps/seam_a.map and seam_b.map:
//
//   A (at 0,0):   room [0,20]x[10,30], corridor [20,40]x[18,22], open at x=40
//   B (at 40,0):  corridor [0,20]x[18,22] open at x=0, room [20,40]x[10,30]
//
#include <cstdio>

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

static constexpr fx F(i64 n) { return fx::from_int(n); }
// For printing only: the core itself has no floating point.
static double D(fx v) { return static_cast<double>(v.raw) / 4294967296.0; }

// A room with a corridor leaving it east (`east_room` false: map A) or a
// corridor arriving from the west into a room (true: map B). The open end of
// the corridor is a LEDGE, which is all a map has to say to be joinable.
static void build(MapStorage& m, bool east_room) {
    m.clear();
    if (!east_room) {
        const fx2 room[] = {{F(0), F(10)}, {F(20), F(10)}, {F(20), F(18)}, {F(20), F(22)},
                            {F(20), F(30)}, {F(0), F(30)}};
        const fx2 hall[] = {{F(20), F(18)}, {F(40), F(18)}, {F(40), F(22)}, {F(20), F(22)}};
        m.add_sector(room, 6, F(0), F(8), 0);
        m.add_sector(hall, 4, F(0), F(8), 0);
        m.add_side(0, 0, 1, NO_SECTOR, 0);
        m.add_side(0, 1, 2, NO_SECTOR, 0);
        m.add_side(0, 2, 3, 1, 0);
        m.add_side(0, 3, 4, NO_SECTOR, 0);
        m.add_side(0, 4, 5, NO_SECTOR, 0);
        m.add_side(0, 5, 0, NO_SECTOR, 0);
        m.add_side(1, 0, 1, NO_SECTOR, 0);
        m.add_side(1, 1, 2, NO_SECTOR, side_flag::LEDGE);   // the open end, x = 40
        m.add_side(1, 2, 3, NO_SECTOR, 0);
        m.add_side(1, 3, 0, 0, 0);
    } else {
        const fx2 hall[] = {{F(0), F(18)}, {F(20), F(18)}, {F(20), F(22)}, {F(0), F(22)}};
        const fx2 room[] = {{F(20), F(10)}, {F(40), F(10)}, {F(40), F(30)}, {F(20), F(30)},
                            {F(20), F(22)}, {F(20), F(18)}};
        m.add_sector(hall, 4, F(0), F(8), 0);
        m.add_sector(room, 6, F(0), F(8), 0);
        m.add_side(0, 0, 1, NO_SECTOR, 0);
        m.add_side(0, 1, 2, 1, 0);
        m.add_side(0, 2, 3, NO_SECTOR, 0);
        m.add_side(0, 3, 0, NO_SECTOR, side_flag::LEDGE);   // the open end, x = 0
        m.add_side(1, 0, 1, NO_SECTOR, 0);
        m.add_side(1, 1, 2, NO_SECTOR, 0);
        m.add_side(1, 2, 3, NO_SECTOR, 0);
        m.add_side(1, 3, 4, NO_SECTOR, 0);
        m.add_side(1, 4, 5, 0, 0);
        m.add_side(1, 5, 0, NO_SECTOR, 0);
    }
    m.rebuild_grid();
}

// Stores are two megabytes each: static, never on the stack.
static MapStorage g_a, g_b;
static MapStorage g_world, g_world2;
static World g_w, g_w2;
static MapData g_view, g_view2;

static void attach(World& w, MapStorage& s, MapData& v) {
    v = s.view();
    w.set_map_storage(&s);
    w.set_map(&v);
}

static const fx2 AT_A{F(0), F(0)}, AT_B{F(40), F(0)};

// Walk east from A's room for up to `ticks`, reporting what happened.
struct Walk {
    fx   max_x{};
    fx   min_z{};
    bool left_sector = false;   // ever had no sector after settling
    u32  last_chunk = NO_CHUNK;
};
static Walk walk_east(World& w, const MapStorage& s, u32 a, int ticks) {
    Walk out;
    out.min_z = F(1000);
    proto::InputFrame east;
    east.move_x = 32767;
    for (int t = 0; t < ticks; ++t) {
        w.set_input(a, east);
        w.step();
        const AgentState& b = w.agent(a);
        if (b.pos.x > out.max_x) out.max_x = b.pos.x;
        if (b.pos.z < out.min_z) out.min_z = b.pos.z;
        if (b.mv.sector < 0) out.left_sector = true;
        else out.last_chunk = s.chunk_of_sector(static_cast<u32>(b.mv.sector));
        if (b.pos.x > F(70)) break;
    }
    return out;
}

static u32 settle(World& w, fx3 at) {
    const u32 a = w.spawn(at);
    for (int i = 0; i < 30; ++i) { w.set_input(a, proto::InputFrame{}); w.step(); }
    return a;
}

static void test_two_maps_join_at_their_open_ends() {
    std::printf("chunks: two maps placed edge to edge join where their open ends meet\n");
    g_world.clear();
    CHECK(g_world.add_chunk(0, g_a, AT_A) != NO_CHUNK);
    CHECK(g_world.add_chunk(1, g_b, AT_B) != NO_CHUNK);
    CHECK(g_world.stitch_count() == 1);
    CHECK(g_world.sector_count() == 4);
    CHECK(g_world.chunk_of_sector(0) == 0 && g_world.chunk_of_sector(3) == 1);

    // CONTROL: one unit out of place, and nothing joins. A join that did not
    // need the edges to meet would join maps that do not.
    g_world2.clear();
    g_world2.add_chunk(0, g_a, AT_A);
    g_world2.add_chunk(1, g_b, fx2{F(41), F(0)});
    CHECK(g_world2.stitch_count() == 0);

    // Refusals: the same key twice, and a store that is already a plain map.
    CHECK(g_world.add_chunk(1, g_b, AT_B) == NO_CHUNK);
    CHECK(g_world2.find_chunk(1) != nullptr);
    static MapStorage plain;
    build(plain, false);
    CHECK(plain.add_chunk(5, g_b, AT_B) == NO_CHUNK);
}

static void test_a_body_walks_from_one_into_the_other() {
    std::printf("chunks: a body walks from one map into the next like between two rooms\n");
    g_world.clear();
    g_world.add_chunk(0, g_a, AT_A);
    g_world.add_chunk(1, g_b, AT_B);
    g_w.reset();
    attach(g_w, g_world, g_view);
    const u32 a = settle(g_w, fx3{F(10), F(20), F(1)});
    const Walk r = walk_east(g_w, g_world, a, 900);
    std::printf("  reached x=%.2f, lowest z=%.2f, ended in chunk %u\n",
                D(r.max_x), D(r.min_z), r.last_chunk);
    CHECK(r.max_x > F(70));
    CHECK(!r.left_sector);
    CHECK(r.min_z > F(-1));
    CHECK(r.last_chunk == 1);

    // CONTROL: the same walk with a five-unit gap before the second map. The
    // corridor now ends in a ledge, and the body walks off it and falls. (A
    // one-unit gap is not a control: the body dips a fraction into it and
    // steps out the far side, which is a fall nobody would see.)
    g_world2.clear();
    g_world2.add_chunk(0, g_a, AT_A);
    g_world2.add_chunk(1, g_b, fx2{F(45), F(0)});
    g_w2.reset();
    attach(g_w2, g_world2, g_view2);
    const u32 b = settle(g_w2, fx3{F(10), F(20), F(1)});
    const Walk c = walk_east(g_w2, g_world2, b, 900);
    std::printf("  CONTROL reached x=%.2f, lowest z=%.2f\n", D(c.max_x), D(c.min_z));
    // (It goes on east through the void under B -- nothing down there is solid
    // -- so how far it got says nothing; that it fell says everything.)
    CHECK(c.left_sector && c.min_z < F(-1));
}

static void test_maps_come_and_go_underneath() {
    std::printf("chunks: a map removed and added back, into different slots, still joins\n");
    g_world.clear();
    g_world.add_chunk(0, g_a, AT_A);
    g_world.add_chunk(1, g_b, AT_B);
    g_w.reset();
    attach(g_w, g_world, g_view);
    const u32 a = settle(g_w, fx3{F(10), F(20), F(1)});

    // A moving sector in the map about to go, so its motion has to go too.
    const u32 b_hall = g_world.find_chunk(1)->sectors.first;
    CHECK(g_w.motions().add(g_world, b_hall) < MAX_MOVING_SECTORS);
    CHECK(g_w.motions().count() == 1);

    CHECK(g_world.remove_chunk(1));
    g_w.motions().forget_sectors(b_hall, 2);
    attach(g_w, g_world, g_view);
    CHECK(g_w.motions().count() == 0);
    CHECK(g_world.stitch_count() == 0);
    CHECK(g_world.find_chunk(1) == nullptr);
    CHECK(g_world.chunk_of_sector(b_hall) == NO_CHUNK);
    // A's open end is open again, exactly: a ledge with no neighbour.
    {
        const MapData v = g_world.view();
        const Sector& hall = v.sector(1);
        const Side& end = v.side(static_cast<u32>(hall.first_side) + 1);
        CHECK((end.flags & side_flag::LEDGE) && end.neighbor == NO_SECTOR &&
              !(end.flags & side_flag::STITCHED));
    }

    // Something else arrives first and takes B's old slots, so B comes back
    // somewhere else in every array. Nothing about the walk may notice.
    CHECK(g_world.add_chunk(7, g_a, fx2{F(400), F(0)}) != NO_CHUNK);
    CHECK(g_world.add_chunk(1, g_b, AT_B) != NO_CHUNK);
    attach(g_w, g_world, g_view);
    CHECK(g_world.find_chunk(1)->sectors.first != b_hall);
    CHECK(g_world.stitch_count() == 1);
    const Walk r = walk_east(g_w, g_world, a, 900);
    CHECK(r.max_x > F(70) && !r.left_sector && r.last_chunk == 1);
}

static void test_two_holders_of_different_sets_agree_on_motion() {
    std::printf("chunks: two worlds holding different maps compute the same motion\n");
    // The server holds everybody's neighbourhood and a client only its own, so
    // the same two maps sit at different indices in the two stores. What must
    // agree is the body; what will not is the sector NUMBER.
    g_world.clear();
    g_world.add_chunk(0, g_a, AT_A);
    g_world.add_chunk(1, g_b, AT_B);
    g_world2.clear();
    g_world2.add_chunk(9, g_b, fx2{F(400), F(400)});   // somebody else's, far away
    g_world2.add_chunk(0, g_a, AT_A);
    g_world2.add_chunk(1, g_b, AT_B);
    g_w.reset();
    g_w2.reset();
    attach(g_w, g_world, g_view);
    attach(g_w2, g_world2, g_view2);
    const u32 a = settle(g_w, fx3{F(10), F(20), F(1)});
    const u32 b = settle(g_w2, fx3{F(10), F(20), F(1)});

    proto::InputFrame east;
    east.move_x = 32767;
    bool same_motion = true, numbered_differently = false, hash_differs = false;
    for (int t = 0; t < 700; ++t) {
        g_w.set_input(a, east);
        g_w2.set_input(b, east);
        g_w.step();
        g_w2.step();
        const AgentState& p = g_w.agent(a);
        const AgentState& q = g_w2.agent(b);
        if (!(p.pos == q.pos) || !(p.vel == q.vel)) same_motion = false;
        if (p.mv.sector != q.mv.sector) numbered_differently = true;
        if (g_w.agent_hash(a) != g_w2.agent_hash(b)) hash_differs = true;
    }
    CHECK(same_motion);
    // CONTROL: the layouts really differ, or the agreement above is two copies
    // of one store agreeing with itself.
    CHECK(numbered_differently);
    // AND THE PREDICTION COMPARISON AGREES, because it hashes sector NAMES. It
    // hashed the index until this was written, and then every tick of this
    // walk was a reported mispredict between two worlds that agreed exactly.
    CHECK(!hash_differs);
}

static void test_names_travel_indices_do_not() {
    std::printf("chunks: a sector's name is the same wherever it is held\n");
    g_world.clear();
    g_world.add_chunk(0, g_a, AT_A);
    g_world.add_chunk(1, g_b, AT_B);
    g_world2.clear();
    g_world2.add_chunk(9, g_b, fx2{F(400), F(400)});
    g_world2.add_chunk(0, g_a, AT_A);
    g_world2.add_chunk(1, g_b, AT_B);
    // B's room is local sector 1 of chunk 1, in both.
    const i32 here = static_cast<i32>(g_world.find_chunk(1)->sectors.first + 1);
    const i32 there = static_cast<i32>(g_world2.find_chunk(1)->sectors.first + 1);
    CHECK(here != there);                                        // CONTROL
    CHECK(g_world.sector_name(here) == ((1u << 16) | 1u));
    CHECK(g_world.sector_name(here) == g_world2.sector_name(there));
    CHECK(g_world2.sector_by_name(g_world.sector_name(here)) == there);
    // Sides the same way.
    const i32 side = static_cast<i32>(g_world.find_chunk(1)->sides.first + 3);
    CHECK(g_world2.side_by_name(g_world.side_name(side)) ==
          static_cast<i32>(g_world2.find_chunk(1)->sides.first + 3));
    // Not resident, and nonsense, are both "nowhere".
    CHECK(g_world.sector_by_name((9u << 16) | 0u) == NO_SECTOR);
    CHECK(g_world.sector_by_name(NO_NAME) == NO_SECTOR);
    CHECK(g_world.sector_name(NO_SECTOR) == NO_NAME);
    // A plain one-map store: the name IS the index, so nothing that predates
    // chunks changes on the wire or in the hash.
    CHECK(g_a.sector_name(1) == 1u && g_a.sector_by_name(1) == 1);
    CHECK(g_a.sector_name(5) == NO_NAME);
    // Keys past the limit are refused: the name must fit an i32.
    static MapStorage big;
    big.clear();
    CHECK(big.add_chunk(MAX_CHUNK_KEY + 1, g_a, AT_A) == NO_CHUNK);
    CHECK(big.add_chunk(MAX_CHUNK_KEY, g_a, AT_A) != NO_CHUNK);
}

int main() {
    build(g_a, false);
    build(g_b, true);
    test_two_maps_join_at_their_open_ends();
    test_a_body_walks_from_one_into_the_other();
    test_maps_come_and_go_underneath();
    test_two_holders_of_different_sets_agree_on_motion();
    test_names_travel_indices_do_not();
    std::printf("\nchunks: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
