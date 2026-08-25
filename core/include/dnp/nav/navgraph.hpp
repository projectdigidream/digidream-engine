#pragma once
//
// The sector navigation graph (design §3.2.4).
//
// Sectors are nodes; shared sides and in-map portals are edges. That is the
// whole structure, and it falls out of the map rather than being authored
// alongside it -- a level designer who moves a wall does not also have to
// remember to move a navmesh.
//
// The one rule worth stating up front: **whether an edge is walkable is decided
// by the same predicate the mover uses.** nav asks
// physics::sector_admits(), which is what side_is_passable() asks, which is
// what resolves collision. An enemy therefore cannot path through a door it
// would be stopped by, and cannot refuse a step it would in fact walk up. Two
// implementations of "walkable" would drift apart within a phase, and the
// symptom -- enemies pressing into walls, or ignoring a route players use every
// round -- looks like an AI bug rather than a duplication bug.
//
// Design §3.2.2 says enemies do not jump. Nothing here implements that: it
// falls out of asking sector_admits(), which refuses a rise over STEP_HEIGHT.
//
// Walkability is evaluated at QUERY time, not baked. A lift connects two rooms
// at the bottom of its travel and not at the top, and a graph that baked the
// answer would be wrong for one of those on every map that has a lift.
//
#include "dnp/geom/mapstore.hpp"
#include "dnp/physics/collide.hpp"
#include "dnp/sim/state_hash.hpp"

namespace dnp::nav {

using namespace dnp;

inline constexpr u32 MAX_NAV_NODES   = geom::MAX_MAP_SECTORS;
inline constexpr u32 MAX_NAV_PORTALS = 64;
inline constexpr u32 MAX_NAV_EDGES   = geom::MAX_MAP_SIDES + MAX_NAV_PORTALS;

struct NavEdge {
    u32 to = 0;
    i32 side = -1;   // index into MapData::sides, or -1 for a portal edge
    fx  width{};     // length of the shared boundary; a narrow door excludes a wide body
    fx  cost{};      // centroid to centroid
};

struct NavNode {
    fx2 centroid{};
    u32 first_edge = 0;
    u32 edge_count = 0;
};

// The body a path is planned for. Not a MoveParams: pathing needs two of its
// fields and copying the whole movement table into the query would invite
// someone to make routing depend on dash cooldown.
struct NavBody {
    fx radius = fx::from_frac(1, 2);
    fx height = fx::from_int(2);
};

class NavGraph {
public:
    // Build every node and edge from scratch.
    bool build(const geom::MapData& map) noexcept;

    // Recompute one sector's geometry-derived values in place, for §3.1.1's
    // staged shape commit. Cheap and local because a shape edit moves points
    // and never adds or removes a side, so the topology -- which node has which
    // edges, and where they live in the array -- cannot change. Only centroids,
    // widths and costs can, and a moved sector changes its neighbours' costs
    // too, so those are refreshed as well.
    //
    // Returns false if the topology HAS changed underneath it, rather than
    // silently rebuilding a graph that no longer describes the map.
    bool rebuild_sector(const geom::MapData& map, u32 s) noexcept;

    // A directed link between two sectors that share no boundary (design
    // §3.2.4, "in-map portals as edges"). Recorded and then folded into the
    // edge array by a full rebuild, so a query never has to consider two kinds
    // of adjacency. A load-time operation; Phase 7 is what fills it in.
    bool link_portal(const geom::MapData& map, u32 from, u32 to) noexcept;

    void clear() noexcept;

    u32 node_count() const noexcept { return node_count_; }
    const NavNode& node(u32 i) const noexcept { return nodes_[i]; }
    const NavEdge& edge(u32 i) const noexcept { return edges_[i]; }
    u32 edge_count() const noexcept { return edge_count_; }
    u32 portal_count() const noexcept { return portal_count_; }

    // Can a body cross this edge, given the map as it is right now?
    bool edge_is_open(const geom::MapData& map, u32 from, const NavEdge& e,
                      const NavBody& body) const noexcept;

    u64 hash() const noexcept;

private:
    bool build_edges(const geom::MapData& map) noexcept;
    void compute_node(const geom::MapData& map, u32 s) noexcept;

    NavNode nodes_[MAX_NAV_NODES]{};
    NavEdge edges_[MAX_NAV_EDGES]{};
    u32     node_count_ = 0;
    u32     edge_count_ = 0;

    struct Portal { u32 from = 0; u32 to = 0; };
    Portal portals_[MAX_NAV_PORTALS]{};
    u32    portal_count_ = 0;
};

enum class PathStatus : u8 {
    Ok = 0,
    SameSector,      // already there; the path is one node long
    BadSector,       // an index outside the map
    Unreachable,     // the graph is connected differently, or not at all
    PathTooLong,     // a route exists but does not fit in the caller's buffer
};

const char* path_status_text(PathStatus s) noexcept;

struct PathResult {
    PathStatus status = PathStatus::Unreachable;
    u32        length = 0;   // nodes written to out_path
    fx         cost{};       // total edge cost of the route
    u32        expanded = 0; // nodes popped; a cost measure, not a result
};

// Search scratch. Caller-owned because core/ allocates nothing, and around
// 20 KB -- which is a third of Emscripten's default stack, so this belongs in
// static or heap storage on every target rather than as a local.
struct NavQuery {
    fx   g[MAX_NAV_NODES]{};
    fx   f[MAX_NAV_NODES]{};
    i32  came_from[MAX_NAV_NODES]{};
    u8   closed[MAX_NAV_NODES]{};
    u8   opened[MAX_NAV_NODES]{};
    u32  heap[MAX_NAV_EDGES + MAX_NAV_NODES]{};
    u32  heap_size = 0;
};

// A* over the sector graph.
//
// `out_path` receives sector indices from `from` to `to` inclusive. Ties break
// toward the lower sector index, so two peers running the same query get the
// same route rather than merely an equally good one.
PathResult find_path(const geom::MapData& map, const NavGraph& g, NavQuery& q,
                     u32 from, u32 to, const NavBody& body,
                     u32* out_path, u32 max_path) noexcept;

}  // namespace dnp::nav
