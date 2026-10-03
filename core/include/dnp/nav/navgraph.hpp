#pragma once
//
// The sector navigation graph (design §3.2.4).
//
// Sectors are nodes; shared sides and in-map portals are edges. That is the
// whole structure, and it falls out of the map rather than being authored
// alongside it -- a level designer who moves a wall does not also have to
// remember to move a navmesh.
//
// ADJACENCY IS NOT ONLY WHAT THE MAP SPELLS OUT. A side is `wall`,
// `neighbor N`, or `open`, and only `neighbor` names the sector beyond it.
// Hand-authored maps use `open` almost throughout -- maps/mylevel.map is five
// sectors and every edge is `open` -- so a neighbour-only graph made every
// sector an island. build_edges therefore also DERIVES adjacency across `open`
// sides by asking find_sector_at what is a hair past them. Walls are left
// alone: `wall` and `open` are separate words in the format precisely because
// one is a barrier and the other is a fall.
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
// Design §3.2.2 says enemies do not jump, and that is still the DEFAULT: a
// NavBody with jump_height zero routes exactly as it always did, because the
// only question asked of it is sector_admits(), which refuses a rise over
// STEP_HEIGHT.
//
// A body that CAN hop states how high, and the graph then also considers rises
// it could clear. That is a second walkability rule and not a second copy of
// the first: walking still asks the mover's own predicate, and the jump rule
// applies only where that predicate has already said no. Jumping is a
// capability the mover has no predicate for, so nav adds one rather than
// paraphrasing an existing one.
//
// THE HALF THAT IS NOT HERE: a route through a jump is a promise the caller
// has to keep. find_path reports which steps need one (out_jump), and a runtime
// that ignores it sends monsters to walk into a ledge forever -- which is
// strictly worse than never routing them there at all.
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
// Twice the sides, because a side can carry an explicit edge OR a derived one
// and a sector's `open` edges may reach several distinct neighbours. Derived
// edges are deduplicated per node, so the real count is far below this; the
// headroom is here so a legal map cannot fail to build.
inline constexpr u32 MAX_NAV_EDGES   = geom::MAX_MAP_SIDES * 2 + MAX_NAV_PORTALS;

struct NavEdge {
    u32 to = 0;
    i32 side = -1;   // index into MapData::sides, or -1 for a portal edge
    fx  width{};     // length of the shared boundary; a narrow door excludes a wide body
    fx  cost{};      // centroid to centroid

    // Was this edge DERIVED from the geometry rather than declared by a
    // `neighbor`? Recorded rather than re-inferred, because the only way to
    // infer it is to read the side's neighbour out of the map -- and
    // rebuild_sector's whole job is to notice when that is exactly what
    // changed. Inferring it made severing a boundary invisible to the
    // topology check.
    bool derived = false;
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

    // How much MORE than a step this body can gain, if it jumps. Zero is
    // design §3.2.2's rule and the default, so nothing routes differently
    // unless somebody asks for it.
    //
    // A height rather than an impulse: what routing needs to know is how high
    // it can get, and deriving that from jump_impulse and gravity_scale here
    // would put two of MoveParams' fields into the nav query and invite a
    // third. The caller owns the conversion and states the answer.
    fx jump_height = fx::zero();
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
    //
    // `needs_jump`, when given, says whether crossing requires one -- true only
    // when the walk rule refused and the jump rule allowed. A caller that plans
    // a route through such an edge has to actually jump there.
    bool edge_is_open(const geom::MapData& map, u32 from, const NavEdge& e,
                      const NavBody& body,
                      bool* needs_jump = nullptr) const noexcept;

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
    GaveUp,          // the search ran out of the caller's `max_expand`
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
    u8   came_jump[MAX_NAV_NODES]{};   // did the parent link need a jump
    u8   closed[MAX_NAV_NODES]{};
    u8   opened[MAX_NAV_NODES]{};
    u32  heap[MAX_NAV_EDGES + MAX_NAV_NODES]{};
    u32  heap_size = 0;
    // At most this many nodes expanded, then GaveUp; 0 is no limit. For a
    // caller asking on a schedule -- a monster -- on a graph as big as a whole
    // region: a target that cannot be reached otherwise costs a search of
    // EVERY node, every time it is asked (measured on DOOMWORLD: 7000 nodes,
    // over a thousand times in half a minute, ticks of 40-90 ms).
    u32  max_expand = 0;
};

// A* over the sector graph.
//
// `out_path` receives sector indices from `from` to `to` inclusive. Ties break
// toward the lower sector index, so two peers running the same query get the
// same route rather than merely an equally good one.
// `out_jump`, when given, is filled alongside out_path: out_jump[i] is true if
// getting from node i-1 to node i needs a jump. out_jump[0] is always false --
// nobody jumps to arrive where they already are.
PathResult find_path(const geom::MapData& map, const NavGraph& g, NavQuery& q,
                     u32 from, u32 to, const NavBody& body,
                     u32* out_path, u32 max_path,
                     bool* out_jump = nullptr) noexcept;

}  // namespace dnp::nav
