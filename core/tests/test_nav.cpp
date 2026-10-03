//
// The sector navigation graph (§3.2.4).
//
// The claim under test is not "A* finds a path" -- that is the easy half and it
// would pass with a walkability rule invented here. It is that nav and the
// mover agree about what is walkable, because they ask the same function. So
// the central test compares every edge in the graph against what the collision
// code would allow, and the rest check that the answer is recomputed when the
// world moves rather than baked when the map loaded.
//
#include <cstdio>
#include <memory>

#include "dnp/nav/navgraph.hpp"

using namespace dnp;
using namespace dnp::geom;
using namespace dnp::nav;

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

// A 2x2 block of ten-unit rooms. Two routes from 0 to 3, of exactly equal cost,
// which is what makes the tie-break testable rather than incidental.
//
//     +----+----+          y
//     | 2  | 3  |          ^
//     +----+----+          |
//     | 0  | 1  |          +--> x
//     +----+----+
struct Grid {
    MapStorage store;
    MapData    view{};

    // `hub_north_first` authors sector 0's two doorways in the opposite order.
    // The geometry is identical; only the order the sides were added differs,
    // which is the thing a route must not depend on.
    explicit Grid(bool hub_north_first = false) {
        const fx2 s0[] = {{F(0), F(0)}, {F(10), F(0)}, {F(10), F(10)}, {F(0), F(10)}};
        const fx2 s1[] = {{F(10), F(0)}, {F(20), F(0)}, {F(20), F(10)}, {F(10), F(10)}};
        const fx2 s2[] = {{F(0), F(10)}, {F(10), F(10)}, {F(10), F(20)}, {F(0), F(20)}};
        const fx2 s3[] = {{F(10), F(10)}, {F(20), F(10)}, {F(20), F(20)}, {F(10), F(20)}};
        store.add_sector(s0, 4, F(0), F(8), 1);
        store.add_sector(s1, 4, F(0), F(8), 2);
        store.add_sector(s2, 4, F(0), F(8), 3);
        store.add_sector(s3, 4, F(0), F(8), 4);

        store.add_side(0, 0, 1, NO_SECTOR, 0);
        if (hub_north_first) {
            store.add_side(0, 2, 3, 2, 0);   // north, to the HIGHER-indexed room
            store.add_side(0, 1, 2, 1, 0);   // east
        } else {
            store.add_side(0, 1, 2, 1, 0);
            store.add_side(0, 2, 3, 2, 0);
        }
        store.add_side(0, 3, 0, NO_SECTOR, 0);

        store.add_side(1, 0, 1, NO_SECTOR, 0);
        store.add_side(1, 1, 2, NO_SECTOR, 0);
        store.add_side(1, 2, 3, 3, 0);
        store.add_side(1, 3, 0, 0, 0);

        store.add_side(2, 0, 1, 0, 0);
        store.add_side(2, 1, 2, 3, 0);
        store.add_side(2, 2, 3, NO_SECTOR, 0);
        store.add_side(2, 3, 0, NO_SECTOR, 0);

        store.add_side(3, 0, 1, 1, 0);
        store.add_side(3, 1, 2, NO_SECTOR, 0);
        store.add_side(3, 2, 3, NO_SECTOR, 0);
        store.add_side(3, 3, 0, 2, 0);
        refresh();
    }
    void refresh() { view = store.view(); }
    void set_floor(u32 s, fx z) { store.sector_mut(s).floor_z = z; refresh(); }
};

static NavQuery g_query;   // ~20 KB: static, never a stack local (see the header)
static u32      g_path[MAX_NAV_NODES];

// ------------------------------------------------------------------ structure

static void test_the_graph_is_the_map() {
    std::printf("nav: the graph is derived from the map, not authored beside it\n");

    static Grid grid;
    const auto g_owned = std::make_unique<NavGraph>();
    NavGraph& g = *g_owned;
    CHECK(g.build(grid.view));
    CHECK(g.node_count() == 4);
    // Eight directed edges: four shared boundaries, each seen from both sides.
    CHECK(g.edge_count() == 8);

    // Centroids are room centres, so a route cost is a real distance.
    CHECK(g.node(0).centroid == (fx2{F(5), F(5)}));
    CHECK(g.node(3).centroid == (fx2{F(15), F(15)}));

    // Every edge is reciprocated. A one-way boundary would let an enemy walk
    // somewhere it could never leave, and nothing in the map says so.
    u32 reciprocated = 0;
    for (u32 s = 0; s < g.node_count(); ++s) {
        for (u32 k = 0; k < g.node(s).edge_count; ++k) {
            const NavEdge& e = g.edge(g.node(s).first_edge + k);
            const NavNode& back = g.node(e.to);
            for (u32 j = 0; j < back.edge_count; ++j) {
                if (g.edge(back.first_edge + j).to == s) { ++reciprocated; break; }
            }
        }
    }
    CHECK(reciprocated == 8);
}

// THE test. nav's idea of walkable and the mover's must be the same idea.
static void test_walkability_is_the_movers_walkability() {
    std::printf("nav: an edge is open exactly when the mover would let a body cross it\n");

    static Grid grid;
    const auto g_owned = std::make_unique<NavGraph>();
    NavGraph& g = *g_owned;
    g.build(grid.view);
    const NavBody body;

    int compared = 0;
    int disagreed = 0;
    int open = 0;

    // Sweep the floor of sector 1 through every interesting height: level,
    // exactly at the step threshold, just over it, and far above. A single
    // configuration would agree by luck.
    const fx heights[] = {F(0), fx::from_frac(1, 4), sim::STEP_HEIGHT,
                          sim::STEP_HEIGHT + fx::eps(), F(1), F(4)};
    for (fx h : heights) {
        grid.set_floor(1, h);
        g.build(grid.view);   // rebuild: the graph must not cache the answer

        for (u32 s = 0; s < g.node_count(); ++s) {
            for (u32 k = 0; k < g.node(s).edge_count; ++k) {
                const NavEdge& e = g.edge(g.node(s).first_edge + k);
                const bool nav_says = g.edge_is_open(grid.view, s, e, body);
                const bool mover_says = physics::side_is_passable(
                    grid.view, static_cast<u32>(e.side), s,
                    grid.view.sector(s).floor_z, body.height);
                ++compared;
                if (nav_says) ++open;
                if (nav_says != mover_says) ++disagreed;
            }
        }
    }
    std::printf("  compared %d edges across %zu floor heights, %d open, %d disagreements\n",
                compared, sizeof(heights) / sizeof(heights[0]), open, disagreed);
    CHECK(disagreed == 0);
    // Both extremes actually occurred, or "they agree" would mean "both always
    // said no".
    CHECK(open > 0);
    CHECK(open < compared);
    grid.set_floor(1, F(0));
}

// --------------------------------------------------------------------- search

static void test_routes_and_tie_breaking() {
    std::printf("nav: A* routes, and breaks ties the same way every time\n");

    static Grid grid;
    const auto g_owned = std::make_unique<NavGraph>();
    NavGraph& g = *g_owned;
    g.build(grid.view);
    const NavBody body;

    PathResult r = find_path(grid.view, g, g_query, 0, 3, body, g_path, MAX_NAV_NODES);
    CHECK(r.status == PathStatus::Ok);
    CHECK(r.length == 3);
    CHECK(r.cost == F(20));
    // Both routes cost exactly 20. The lower-index one wins, so two identical
    // enemies do not take different corridors for no visible reason.
    CHECK(g_path[0] == 0 && g_path[1] == 1 && g_path[2] == 3);
    std::printf("  0 -> 3 via %u,%u,%u at cost %.1f (%u expanded)\n",
                g_path[0], g_path[1], g_path[2], D(r.cost), r.expanded);
    // Each node is expanded at most once. The open set holds duplicates by
    // design -- there is no decrease-key -- so this is what says the stale ones
    // are actually skipped rather than re-explored.
    CHECK(r.expanded <= g.node_count());

    // The same geometry, with sector 0's two doorways authored in the opposite
    // order. A* explores edges in array order, so if the tie-break were "keep
    // whichever arrived first" this would route through 2 instead -- and two
    // servers running the same map would disagree about a route for no reason
    // a player could ever see.
    static Grid reordered(true);
    const auto g2_owned = std::make_unique<NavGraph>();
    NavGraph& g2 = *g2_owned;
    g2.build(reordered.view);
    u32 other[MAX_NAV_NODES];
    const PathResult r_other =
        find_path(reordered.view, g2, g_query, 0, 3, body, other, MAX_NAV_NODES);
    CHECK(r_other.status == PathStatus::Ok);
    CHECK(r_other.length == r.length);
    CHECK(r_other.cost == r.cost);
    CHECK(other[0] == 0 && other[1] == 1 && other[2] == 3);
    // ...and the reordering really did change which edge is examined first, or
    // the check above compares two identical graphs.
    CHECK(g2.edge(g2.node(0).first_edge).to == 2);
    CHECK(g.edge(g.node(0).first_edge).to == 1);
    std::printf("  authored the other way round, still 0,%u,%u\n", other[1], other[2]);

    // Same query, same answer, every time.
    for (int i = 0; i < 16; ++i) {
        u32 again[MAX_NAV_NODES];
        const PathResult r2 = find_path(grid.view, g, g_query, 0, 3, body, again, MAX_NAV_NODES);
        CHECK(r2.status == r.status && r2.length == r.length && r2.cost == r.cost);
        for (u32 k = 0; k < r.length; ++k) CHECK(again[k] == g_path[k]);
    }

    CHECK(find_path(grid.view, g, g_query, 2, 2, body, g_path, MAX_NAV_NODES).status
          == PathStatus::SameSector);
    CHECK(find_path(grid.view, g, g_query, 0, 99, body, g_path, MAX_NAV_NODES).status
          == PathStatus::BadSector);
    // A route exists but does not fit: reported, never truncated into a path
    // that leads somewhere else.
    CHECK(find_path(grid.view, g, g_query, 0, 3, body, g_path, 2).status
          == PathStatus::PathTooLong);
}

// The property baking the graph would get wrong.
static void test_a_lift_changes_the_route_without_rebuilding() {
    std::printf("nav: a moving floor changes reachability at query time, not at build time\n");

    static Grid grid;
    const auto g_owned = std::make_unique<NavGraph>();
    NavGraph& g = *g_owned;
    g.build(grid.view);
    const NavBody body;
    const u64 built = g.hash();

    // Raise sector 1 out of reach. The graph is NOT rebuilt.
    grid.set_floor(1, F(4));
    PathResult r = find_path(grid.view, g, g_query, 0, 3, body, g_path, MAX_NAV_NODES);
    CHECK(r.status == PathStatus::Ok);
    CHECK(r.length == 3);
    CHECK(g_path[1] == 2);   // the long way round, through the other room
    std::printf("  with sector 1 raised: 0 -> %u -> 3\n", g_path[1]);

    // Raise the other one too and there is nowhere to go.
    grid.set_floor(2, F(4));
    r = find_path(grid.view, g, g_query, 0, 3, body, g_path, MAX_NAV_NODES);
    CHECK(r.status == PathStatus::Unreachable);

    // Design §3.2.2: enemies do not jump, and nothing here implements that --
    // it falls out of the step rule. A rise of exactly STEP_HEIGHT is walkable;
    // one ulp more is a wall.
    grid.set_floor(1, sim::STEP_HEIGHT);
    CHECK(find_path(grid.view, g, g_query, 0, 1, body, g_path, MAX_NAV_NODES).status
          == PathStatus::Ok);
    grid.set_floor(1, sim::STEP_HEIGHT + fx::eps());
    CHECK(find_path(grid.view, g, g_query, 0, 1, body, g_path, MAX_NAV_NODES).status
          == PathStatus::Unreachable);

    // Through all of that the graph itself never changed: floors are not part
    // of it, which is exactly why it did not need rebuilding.
    CHECK(g.hash() == built);

    grid.set_floor(1, F(0));
    grid.set_floor(2, F(0));
}

// A doorway narrower than the body.
static void test_a_narrow_door_excludes_a_wide_body() {
    std::printf("nav: a body too wide for the gap is not routed through it\n");

    static MapStorage store;
    // A room with a one-unit slot in its east wall, and a corridor beyond it.
    const fx2 room[] = {{F(0), F(0)}, {F(10), F(0)}, {F(10), F(4)},
                        {F(10), F(5)}, {F(10), F(10)}, {F(0), F(10)}};
    const fx2 slot[] = {{F(10), F(4)}, {F(20), F(4)}, {F(20), F(5)}, {F(10), F(5)}};
    store.clear();
    store.add_sector(room, 6, F(0), F(8), 1);
    store.add_sector(slot, 4, F(0), F(8), 2);
    store.add_side(0, 0, 1, NO_SECTOR, 0);
    store.add_side(0, 1, 2, NO_SECTOR, 0);
    store.add_side(0, 2, 3, 1, 0);            // the one-unit gap
    store.add_side(0, 3, 4, NO_SECTOR, 0);
    store.add_side(0, 4, 5, NO_SECTOR, 0);
    store.add_side(0, 5, 0, NO_SECTOR, 0);
    store.add_side(1, 0, 1, NO_SECTOR, 0);
    store.add_side(1, 1, 2, NO_SECTOR, 0);
    store.add_side(1, 2, 3, NO_SECTOR, 0);
    store.add_side(1, 3, 0, 0, 0);
    const MapData view = store.view();

    const auto g_owned = std::make_unique<NavGraph>();

    NavGraph& g = *g_owned;
    CHECK(g.build(view));

    NavBody slim;
    slim.radius = fx::from_frac(4, 10);        // 0.8 across
    NavBody wide;
    wide.radius = fx::from_frac(6, 10);        // 1.2 across

    CHECK(find_path(view, g, g_query, 0, 1, slim, g_path, MAX_NAV_NODES).status == PathStatus::Ok);

    // A BLOCKING side is a wall whatever the floors do, and nav has to see that
    // the same way the mover does.
    store.side_mut(2).flags |= side_flag::BLOCKING;
    const MapData blocked = store.view();
    const auto gb_owned = std::make_unique<NavGraph>();
    NavGraph& gb = *gb_owned;
    gb.build(blocked);
    CHECK(find_path(blocked, gb, g_query, 0, 1, slim, g_path, MAX_NAV_NODES).status
          == PathStatus::Unreachable);
    store.side_mut(2).flags &= static_cast<u8>(~side_flag::BLOCKING);
    // Not "it finds a bad path" -- it correctly finds none. A route through a
    // gap narrower than the body sends an enemy to press against the frame
    // forever, which reads as an AI bug rather than a geometry one.
    CHECK(find_path(view, g, g_query, 0, 1, wide, g_path, MAX_NAV_NODES).status
          == PathStatus::Unreachable);
    std::printf("  a 1.00-unit gap admits a 0.80 body and refuses a 1.20 one\n");
}

static void test_portals_are_edges() {
    std::printf("nav: an in-map portal is an edge like any other\n");

    static Grid grid;
    const auto g_owned = std::make_unique<NavGraph>();
    NavGraph& g = *g_owned;
    g.build(grid.view);
    const NavBody body;

    CHECK(g.portal_count() == 0);
    CHECK(g.link_portal(grid.view, 0, 3));
    CHECK(g.portal_count() == 1);
    CHECK(g.edge_count() == 9);

    const PathResult r = find_path(grid.view, g, g_query, 0, 3, body, g_path, MAX_NAV_NODES);
    CHECK(r.status == PathStatus::Ok);
    CHECK(r.length == 2);
    CHECK(r.cost == fx::zero());
    std::printf("  0 -> 3 in %u hops at cost %.1f\n", r.length, D(r.cost));

    // One-way, as design §3.3 requires. The return trip still walks.
    const PathResult back = find_path(grid.view, g, g_query, 3, 0, body, g_path, MAX_NAV_NODES);
    CHECK(back.status == PathStatus::Ok);
    CHECK(back.length == 3);
    CHECK(back.cost == F(20));
}

// ------------------------------------------------------------- local rebuild

// A portal makes the straight-line heuristic inadmissible, and this is the
// shape of map where that stops being a technicality.
static void test_a_portal_pointing_away_from_the_goal() {
    std::printf("nav: a portal that leads AWAY from the goal is still taken when it is cheaper\n");

    // Five rooms in a row, ten units apart.
    static MapStorage store;
    store.clear();
    for (u32 i = 0; i < 5; ++i) {
        const fx x0 = F(static_cast<i64>(i) * 10);
        const fx x1 = F(static_cast<i64>(i) * 10 + 10);
        const fx2 room[] = {{x0, F(0)}, {x1, F(0)}, {x1, F(10)}, {x0, F(10)}};
        store.add_sector(room, 4, F(0), F(8), static_cast<u16>(i + 1));
    }
    for (u32 i = 0; i < 5; ++i) {
        store.add_side(i, 0, 1, NO_SECTOR, 0);
        store.add_side(i, 1, 2, i + 1 < 5 ? static_cast<i32>(i + 1) : NO_SECTOR, 0);
        store.add_side(i, 2, 3, NO_SECTOR, 0);
        store.add_side(i, 3, 0, i > 0 ? static_cast<i32>(i - 1) : NO_SECTOR, 0);
    }
    const MapData view = store.view();

    const auto g_owned = std::make_unique<NavGraph>();

    NavGraph& g = *g_owned;
    g.build(view);
    const NavBody body;

    // Walking from 0 to 1 costs 10.
    PathResult walk = find_path(view, g, g_query, 0, 1, body, g_path, MAX_NAV_NODES);
    CHECK(walk.status == PathStatus::Ok);
    CHECK(walk.cost == F(10));

    // Now a free route that goes the wrong way first: out to the far end, then
    // back. Cheaper in cost, further in every direction a straight line
    // measures -- which is exactly what an inadmissible heuristic mis-ranks.
    CHECK(g.link_portal(view, 0, 4));
    CHECK(g.link_portal(view, 4, 1));

    const PathResult r = find_path(view, g, g_query, 0, 1, body, g_path, MAX_NAV_NODES);
    CHECK(r.status == PathStatus::Ok);
    // Optimal, not merely valid. Keeping the straight-line heuristic here
    // returns the 10-cost walk instead and reports it as a shortest path.
    CHECK(r.cost == fx::zero());
    CHECK(r.length == 3);
    CHECK(g_path[0] == 0 && g_path[1] == 4 && g_path[2] == 1);
    std::printf("  0 -> 1 the long way round at cost %.1f, beating the %.1f walk\n",
                D(r.cost), D(walk.cost));
}

// A graph where the open set really does end up holding two entries for one
// node, which is the only situation in which skipping the stale one matters.
//
// Written because a plain "expanded <= node_count" assertion on a tidy map is
// vacuous: nothing ever gets re-discovered more cheaply there, so no duplicate
// is ever pushed and the check passes with the skip deleted.
static void test_a_node_is_expanded_once_even_when_re_discovered() {
    std::printf("nav: a node re-discovered more cheaply is still expanded only once\n");

    static MapStorage store;
    store.clear();
    // Rooms of deliberately different widths, so the centroid distances are
    // asymmetric and a detour can genuinely beat the direct route.
    const fx2 s0[] = {{F(0), F(0)}, {F(10), F(0)}, {F(10), F(10)}, {F(0), F(10)}};
    const fx2 s1[] = {{F(10), F(0)}, {F(30), F(0)}, {F(30), F(10)}, {F(10), F(10)}};
    const fx2 s2[] = {{F(30), F(0)}, {F(34), F(0)}, {F(34), F(10)}, {F(30), F(10)}};
    const fx2 s3[] = {{F(10), F(10)}, {F(30), F(10)}, {F(30), F(20)}, {F(10), F(20)}};
    store.add_sector(s0, 4, F(0), F(8), 1);
    store.add_sector(s1, 4, F(0), F(8), 2);
    store.add_sector(s2, 4, F(0), F(8), 3);
    store.add_sector(s3, 4, F(0), F(8), 4);

    store.add_side(0, 0, 1, NO_SECTOR, 0);
    store.add_side(0, 1, 2, 1, 0);
    store.add_side(0, 2, 3, NO_SECTOR, 0);
    store.add_side(0, 3, 0, NO_SECTOR, 0);
    store.add_side(1, 0, 1, NO_SECTOR, 0);
    store.add_side(1, 1, 2, 2, 0);
    store.add_side(1, 2, 3, 3, 0);
    store.add_side(1, 3, 0, 0, 0);
    store.add_side(2, 0, 1, NO_SECTOR, 0);
    store.add_side(2, 1, 2, NO_SECTOR, 0);
    store.add_side(2, 2, 3, NO_SECTOR, 0);
    store.add_side(2, 3, 0, 1, 0);
    store.add_side(3, 0, 1, 1, 0);
    store.add_side(3, 1, 2, NO_SECTOR, 0);
    store.add_side(3, 2, 3, NO_SECTOR, 0);
    store.add_side(3, 3, 0, NO_SECTOR, 0);
    const MapData view = store.view();

    const auto g_owned = std::make_unique<NavGraph>();

    NavGraph& g = *g_owned;
    g.build(view);
    const NavBody body;

    // Without the portal, 0 -> 3 is a straight walk costing 15 + 10.
    PathResult direct = find_path(view, g, g_query, 0, 3, body, g_path, MAX_NAV_NODES);
    CHECK(direct.status == PathStatus::Ok);
    CHECK(direct.cost == F(25));

    // The portal reaches sector 1 more cheaply than the direct edge does, so
    // sector 1 enters the open set at 15 and is then re-discovered at 12 --
    // a second heap entry for a node that already has one.
    CHECK(g.link_portal(view, 0, 2));

    const PathResult r = find_path(view, g, g_query, 0, 3, body, g_path, MAX_NAV_NODES);
    CHECK(r.status == PathStatus::Ok);
    CHECK(r.length == 4);
    CHECK(g_path[0] == 0 && g_path[1] == 2 && g_path[2] == 1 && g_path[3] == 3);
    CHECK(r.cost == F(22));
    // Four nodes, four expansions. The stale entry for sector 1 surfaces after
    // the good one and must be dropped, not re-explored.
    CHECK(r.expanded == 4);
    std::printf("  0 -> 3 costs %.1f (beating the %.1f walk) in %u expansions of %u nodes\n",
                D(r.cost), D(direct.cost), r.expanded, g.node_count());
}

static void test_a_local_rebuild_matches_a_full_one() {
    std::printf("nav: rebuilding one sector matches rebuilding everything (§3.1.1)\n");

    static Grid a;
    static Grid b;
    const auto local_owned = std::make_unique<NavGraph>();
    NavGraph& local = *local_owned;
    const auto full_owned = std::make_unique<NavGraph>();
    NavGraph& full = *full_owned;
    local.build(a.view);

    // Move an outer corner of sector 3, through the staged-commit path the
    // design specifies -- not by writing the point directly.
    const fx2 moved{F(24), F(22)};
    CHECK(a.store.stage_point(3, 2, moved));
    CHECK(b.store.stage_point(3, 2, moved));
    CHECK(a.store.commit_shape(3) == CommitStatus::Ok);
    CHECK(b.store.commit_shape(3) == CommitStatus::Ok);
    a.refresh();
    b.refresh();

    CHECK(local.rebuild_sector(a.view, 3));
    CHECK(full.build(b.view));

    // The whole point of the local path: same answer, less work. If these ever
    // differ, the cheap path is quietly describing a different map -- and the
    // symptom is enemies routing through geometry that moved, which nobody
    // would connect back to a commit.
    CHECK(local.hash() == full.hash());
    std::printf("  local %016llX == full %016llX\n",
                static_cast<unsigned long long>(local.hash()),
                static_cast<unsigned long long>(full.hash()));

    // The centroid really did move, or the comparison above is vacuous.
    CHECK(!(local.node(3).centroid == (fx2{F(15), F(15)})));
    // And the edges POINTING AT sector 3 moved with it. Refreshing only the
    // outbound half leaves 1 -> 3 and 3 -> 1 disagreeing about the distance.
    fx out_cost = fx::zero();
    fx in_cost = fx::from_int(-1);
    for (u32 k = 0; k < local.node(3).edge_count; ++k) {
        const NavEdge& e = local.edge(local.node(3).first_edge + k);
        if (e.to == 1) out_cost = e.cost;
    }
    for (u32 k = 0; k < local.node(1).edge_count; ++k) {
        const NavEdge& e = local.edge(local.node(1).first_edge + k);
        if (e.to == 3) in_cost = e.cost;
    }
    CHECK(out_cost == in_cost);
    CHECK(out_cost != fx::zero());
}

static void test_a_local_rebuild_refuses_a_changed_topology() {
    std::printf("nav: a local rebuild refuses a map whose topology moved under it\n");

    static Grid grid;
    const auto g_owned = std::make_unique<NavGraph>();
    NavGraph& g = *g_owned;
    g.build(grid.view);

    // Sever a boundary. The edge is still in the array, so a rebuild that
    // trusted the caller would leave the graph describing a map nobody
    // authored -- silently, and only visibly much later.
    grid.store.side_mut(1).neighbor = NO_SECTOR;
    grid.refresh();
    CHECK(!g.rebuild_sector(grid.view, 0));

    // The honest response is a full rebuild, which reports the new shape.
    CHECK(g.build(grid.view));
    CHECK(g.edge_count() == 7);
    grid.store.side_mut(1).neighbor = 1;
    grid.refresh();
}

// ADJACENCY ACROSS `open` SIDES.
//
// A side is `wall`, `neighbor N`, or `open`, and only `neighbor` names what is
// beyond it. Hand-authored maps use `open` almost throughout --
// maps/mylevel.map is five sectors and every edge is `open` -- and under the
// neighbour-only rule its graph had ZERO edges: every sector an island, and
// nothing could path anywhere.
//
// Two rooms side by side, sharing the boundary at x = 20, both edges `open`
// and NEITHER naming the other.
struct OpenPair {
    MapStorage store;
    MapData    view{};

    OpenPair(fx east_floor) {
        const fx2 west[] = {{F(0), F(0)}, {F(20), F(0)}, {F(20), F(20)}, {F(0), F(20)}};
        const fx2 east[] = {{F(20), F(0)}, {F(40), F(0)}, {F(40), F(20)}, {F(20), F(20)}};
        store.add_sector(west, 4, F(0), F(20), 1);
        store.add_sector(east, 4, east_floor, east_floor + F(20), 2);
        for (u16 s = 0; s < 2; ++s) {
            for (u16 i = 0; i < 4; ++i) {
                store.add_side(s, i, static_cast<u16>((i + 1) % 4), NO_SECTOR,
                               side_flag::LEDGE);
            }
        }
        refresh();
    }
    void refresh() { view = store.view(); }
};

static bool has_edge(const NavGraph& g, u32 from, u32 to) {
    for (u32 k = 0; k < g.node(from).edge_count; ++k) {
        if (g.edge(g.node(from).first_edge + k).to == to) return true;
    }
    return false;
}

static void test_open_sides_are_adjacency() {
    std::printf("nav: `open` sides connect, though nothing names a neighbour\n");

    static OpenPair flat(fx::zero());
    const auto g_owned = std::make_unique<NavGraph>();
    NavGraph& g = *g_owned;
    CHECK(g.build(flat.view));
    CHECK(g.edge_count() > 0);
    CHECK(has_edge(g, 0, 1));
    CHECK(has_edge(g, 1, 0));

    // And the edge is usable, not merely present.
    NavBody body;
    bool crossable = false;
    for (u32 k = 0; k < g.node(0).edge_count; ++k) {
        const NavEdge& e = g.edge(g.node(0).first_edge + k);
        if (e.to == 1 && g.edge_is_open(flat.view, 0, e, body)) crossable = true;
    }
    CHECK(crossable);

    // THE CONTROL, and it is the whole point: the same two rooms with `wall`
    // instead of `open` are NOT connected. `wall` and `open` are separate
    // words in the format because one is a barrier and the other is a fall,
    // and a nav graph that derived a route through a wall would be overruling
    // the author. Without this, "adjacency is derived from geometry" would
    // read as "every sector touching another is connected".
    static MapStorage walled;
    {
        const fx2 west[] = {{F(0), F(0)}, {F(20), F(0)}, {F(20), F(20)}, {F(0), F(20)}};
        const fx2 east[] = {{F(20), F(0)}, {F(40), F(0)}, {F(40), F(20)}, {F(20), F(20)}};
        walled.add_sector(west, 4, F(0), F(20), 1);
        walled.add_sector(east, 4, F(0), F(20), 2);
        for (u16 s = 0; s < 2; ++s)
            for (u16 i = 0; i < 4; ++i)
                walled.add_side(s, i, static_cast<u16>((i + 1) % 4), NO_SECTOR, 0);
    }
    MapData wv = walled.view();
    const auto gw_owned = std::make_unique<NavGraph>();
    NavGraph& gw = *gw_owned;
    CHECK(gw.build(wv));
    CHECK(!has_edge(gw, 0, 1));
    CHECK(gw.edge_count() == 0);
}

// A drop is one-way. Walking off a platform is free; walking back up is not,
// and nothing here implements that -- it falls out of edge_is_open asking the
// same sector_admits the mover asks, which refuses a rise over STEP_HEIGHT.
static void test_a_derived_drop_is_one_way() {
    std::printf("nav: you can walk off a ledge and not back up it\n");

    // The east room's floor is three units up: far more than a step.
    static OpenPair step_up(F(3));
    const auto g_owned = std::make_unique<NavGraph>();
    NavGraph& g = *g_owned;
    CHECK(g.build(step_up.view));
    // BOTH edges exist -- adjacency is not the same question as walkability.
    CHECK(has_edge(g, 0, 1));
    CHECK(has_edge(g, 1, 0));

    NavBody body;
    bool up = false, down = false;
    for (u32 k = 0; k < g.node(0).edge_count; ++k) {
        const NavEdge& e = g.edge(g.node(0).first_edge + k);
        if (e.to == 1 && g.edge_is_open(step_up.view, 0, e, body)) up = true;
    }
    for (u32 k = 0; k < g.node(1).edge_count; ++k) {
        const NavEdge& e = g.edge(g.node(1).first_edge + k);
        if (e.to == 0 && g.edge_is_open(step_up.view, 1, e, body)) down = true;
    }
    CHECK(!up);      // three units is not a step
    CHECK(down);     // but you can always fall

    // THE CONTROL: the same pair at the same height is crossable BOTH ways, so
    // the refusal above is about the rise and not about derived edges being
    // unusable in general.
    static OpenPair flat(fx::zero());
    const auto g2_owned = std::make_unique<NavGraph>();
    NavGraph& g2 = *g2_owned;
    g2.build(flat.view);
    bool a = false, b = false;
    for (u32 k = 0; k < g2.node(0).edge_count; ++k) {
        const NavEdge& e = g2.edge(g2.node(0).first_edge + k);
        if (e.to == 1 && g2.edge_is_open(flat.view, 0, e, body)) a = true;
    }
    for (u32 k = 0; k < g2.node(1).edge_count; ++k) {
        const NavEdge& e = g2.edge(g2.node(1).first_edge + k);
        if (e.to == 0 && g2.edge_is_open(flat.view, 1, e, body)) b = true;
    }
    CHECK(a);
    CHECK(b);
}

// Derived edges are not duplicated: three probe points along one side must not
// add the same neighbour three times.
static void test_derived_edges_are_deduplicated() {
    std::printf("nav: one edge per neighbour, not one per probe\n");

    static OpenPair flat(fx::zero());
    const auto g_owned = std::make_unique<NavGraph>();
    NavGraph& g = *g_owned;
    g.build(flat.view);

    u32 to_one = 0;
    for (u32 k = 0; k < g.node(0).edge_count; ++k) {
        if (g.edge(g.node(0).first_edge + k).to == 1) ++to_one;
    }
    CHECK(to_one == 1);
}

// A SETTABLE JUMP.
//
// Design §3.2.2 says enemies do not jump, and that is still the default: a
// NavBody with jump_height zero routes exactly as it always did. A body that
// states a height can also cross rises it could clear.
//
// The whole test is the pair. If the jumping body reached the ledge and the
// non-jumping one did too, the field would be doing nothing.
static void test_a_settable_jump_opens_a_ledge() {
    std::printf("nav: a body that can hop crosses what a walker cannot\n");

    // Three units up: six times STEP_HEIGHT, so nothing walks it.
    static OpenPair ledge(F(3));
    const auto g_owned = std::make_unique<NavGraph>();
    NavGraph& g = *g_owned;
    CHECK(g.build(ledge.view));

    const NavEdge* up = nullptr;
    for (u32 k = 0; k < g.node(0).edge_count; ++k) {
        const NavEdge& e = g.edge(g.node(0).first_edge + k);
        if (e.to == 1) up = &e;
    }
    CHECK(up != nullptr);

    NavBody walker;                       // jump_height defaults to zero
    bool hop = false;
    CHECK(!g.edge_is_open(ledge.view, 0, *up, walker, &hop));
    CHECK(!hop);

    // Enough to clear it: the rise is 3 and a step is already worth 0.5.
    NavBody hopper;
    hopper.jump_height = F(3);
    CHECK(g.edge_is_open(ledge.view, 0, *up, hopper, &hop));
    CHECK(hop);                           // and it KNOWS it needed one

    // Not enough. The threshold is a real comparison, not a boolean "can jump".
    NavBody feeble;
    feeble.jump_height = F(1);
    CHECK(!g.edge_is_open(ledge.view, 0, *up, feeble, &hop));

    // And a jump does not turn a walk INTO a jump: an edge the walker already
    // crosses is reported as needing none, or a runtime would jump its way
    // across a flat room.
    static OpenPair flat(fx::zero());
    const auto g2_owned = std::make_unique<NavGraph>();
    NavGraph& g2 = *g2_owned;
    g2.build(flat.view);
    for (u32 k = 0; k < g2.node(0).edge_count; ++k) {
        const NavEdge& e = g2.edge(g2.node(0).first_edge + k);
        if (e.to != 1) continue;
        CHECK(g2.edge_is_open(flat.view, 0, e, hopper, &hop));
        CHECK(!hop);
    }
}

// A jump answers ONE of sector_admits' three refusals. A crawlspace is still a
// crawlspace to something that can jump, and a body that jumped into one would
// end up inside the ceiling.
static void test_a_jump_does_not_open_a_crawlspace() {
    std::printf("nav: jumping does not make a body shorter\n");

    static MapStorage store;
    {
        const fx2 west[] = {{F(0), F(0)}, {F(20), F(0)}, {F(20), F(20)}, {F(0), F(20)}};
        const fx2 east[] = {{F(20), F(0)}, {F(40), F(0)}, {F(40), F(20)}, {F(20), F(20)}};
        store.add_sector(west, 4, F(0), F(20), 1);
        // Floor at the same height, ceiling one unit above it. A two-unit body
        // cannot stand there however high it can jump.
        store.add_sector(east, 4, fx::zero(), F(1), 2);
        for (u16 s = 0; s < 2; ++s)
            for (u16 i = 0; i < 4; ++i)
                store.add_side(s, i, static_cast<u16>((i + 1) % 4), NO_SECTOR,
                               side_flag::LEDGE);
    }
    MapData view = store.view();
    const auto g_owned = std::make_unique<NavGraph>();
    NavGraph& g = *g_owned;
    CHECK(g.build(view));

    NavBody hopper;
    hopper.jump_height = F(10);
    bool hop = false;
    for (u32 k = 0; k < g.node(0).edge_count; ++k) {
        const NavEdge& e = g.edge(g.node(0).first_edge + k);
        if (e.to == 1) CHECK(!g.edge_is_open(view, 0, e, hopper, &hop));
    }

    // THE CONTROL: raise that ceiling and the same body walks in, so the
    // refusal is about the headroom and not about the sector being unreachable.
    store.sector_mut(1).ceil_z = F(20);
    view = store.view();
    const auto g2_owned = std::make_unique<NavGraph>();
    NavGraph& g2 = *g2_owned;
    g2.build(view);
    bool crossed = false;
    for (u32 k = 0; k < g2.node(0).edge_count; ++k) {
        const NavEdge& e = g2.edge(g2.node(0).first_edge + k);
        if (e.to == 1 && g2.edge_is_open(view, 0, e, hopper, &hop)) crossed = true;
    }
    CHECK(crossed);
}

// find_path reports WHICH steps need a jump, because a route through one is a
// promise the caller has to keep.
static void test_the_route_says_where_to_jump() {
    std::printf("nav: the route says which steps need a jump\n");

    static OpenPair ledge(F(3));
    static NavQuery q;
    u32 path[MAX_NAV_NODES];
    bool jump[MAX_NAV_NODES];

    const auto g_owned = std::make_unique<NavGraph>();

    NavGraph& g = *g_owned;
    g.build(ledge.view);

    NavBody hopper;
    hopper.jump_height = F(3);
    const PathResult r = find_path(ledge.view, g, q, 0, 1, hopper, path,
                                   MAX_NAV_NODES, jump);
    CHECK(r.status == PathStatus::Ok);
    CHECK(r.length == 2);
    CHECK(path[0] == 0);
    CHECK(path[1] == 1);
    // Nobody jumps to arrive where they already are.
    CHECK(!jump[0]);
    CHECK(jump[1]);

    // THE CONTROL: a walker has no route at all, so the flag above is not
    // simply set on every step of every route.
    NavBody walker;
    const PathResult r2 = find_path(ledge.view, g, q, 0, 1, walker, path,
                                    MAX_NAV_NODES, jump);
    CHECK(r2.status == PathStatus::Unreachable);

    // And on ground both can walk, the jumper's route is flagged as needing
    // none -- otherwise a runtime would hop across a flat room.
    static OpenPair flat(fx::zero());
    const auto g2_owned = std::make_unique<NavGraph>();
    NavGraph& g2 = *g2_owned;
    g2.build(flat.view);
    const PathResult r3 = find_path(flat.view, g2, q, 0, 1, hopper, path,
                                    MAX_NAV_NODES, jump);
    CHECK(r3.status == PathStatus::Ok);
    CHECK(!jump[0]);
    CHECK(!jump[1]);
}

// A SEARCH CAN BE TOLD TO STOP. A monster asking for a route across a whole
// region graph it cannot cross searched every node, every time it asked; with
// `max_expand` it stops and says so (GaveUp), rather than failing slowly.
static void test_a_search_gives_up_at_its_limit() {
    std::printf("nav: a search with an expansion limit gives up rather than searching on\n");

    static Grid grid;
    const auto g_owned = std::make_unique<NavGraph>();
    NavGraph& g = *g_owned;
    g.build(grid.view);
    const NavBody body;

    const PathResult full = find_path(grid.view, g, g_query, 0, 3, body, g_path, MAX_NAV_NODES);
    CHECK(full.status == PathStatus::Ok && full.expanded > 1);
    g_query.max_expand = 1;
    const PathResult capped = find_path(grid.view, g, g_query, 0, 3, body, g_path, MAX_NAV_NODES);
    CHECK(capped.status == PathStatus::GaveUp);
    CHECK(capped.expanded == 1);
    // CONTROL: a limit the search does not reach changes nothing.
    g_query.max_expand = full.expanded + 1;
    const PathResult roomy = find_path(grid.view, g, g_query, 0, 3, body, g_path, MAX_NAV_NODES);
    CHECK(roomy.status == PathStatus::Ok && roomy.length == full.length && roomy.cost == full.cost);
    g_query.max_expand = 0;
}

int main() {
    test_a_search_gives_up_at_its_limit();
    test_the_graph_is_the_map();
    test_walkability_is_the_movers_walkability();
    test_routes_and_tie_breaking();
    test_a_lift_changes_the_route_without_rebuilding();
    test_a_narrow_door_excludes_a_wide_body();
    test_portals_are_edges();
    test_a_portal_pointing_away_from_the_goal();
    test_a_node_is_expanded_once_even_when_re_discovered();
    test_a_local_rebuild_matches_a_full_one();
    test_a_local_rebuild_refuses_a_changed_topology();

    test_open_sides_are_adjacency();
    test_a_derived_drop_is_one_way();
    test_derived_edges_are_deduplicated();
    test_a_settable_jump_opens_a_ledge();
    test_a_jump_does_not_open_a_crawlspace();
    test_the_route_says_where_to_jump();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
